// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clean-room OnePlus 5T (dumpling) Dash / VOOC STM8S fast-charge MCU driver.
 *
 * Implements I2C bootloader verification/flashing (0x26), bidirectional bit-bang
 * handshake over ap_clk (GPIO 79) / ap_data (GPIO 80), USB D+/D- analog switch
 * safety interlock (GPIO 90/91), power_supply reporting, and /dev/dash ABI.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <uapi/linux/oneplus_dash.h>

#define DASH_FW_NAME			"oneplus/dash_stm8s.bin"
#define DASH_FW_RECORD_SIZE		34
#define DASH_FW_CHUNK_BYTES		16
#define DASH_FW_BASE_ADDR		0x8800
#define DASH_FW_ERASE_BLOCKS		384
#define DASH_FW_READ_BLOCKS		192

#define DASH_REG_ADDR			0x01
#define DASH_REG_WRITE_DATA		0x02
#define DASH_REG_READ_DATA		0x03
#define DASH_REG_ERASE_BLOCK		0x04
#define DASH_REG_COMMIT_WRITE		0x05
#define DASH_REG_JUMP_APP		0x06

#define DASH_CMD_ALLOW			0x02
#define DASH_CMD_REJECT			0x11

#define DASH_WATCHDOG_MS		15000
#define DASH_SAFE_SOC_MIN		1
#define DASH_SAFE_SOC_MAX		85
#define DASH_SAFE_TEMP_MIN_DECIC	165
#define DASH_SAFE_TEMP_MAX_DECIC	430

struct oneplus_dash {
	struct i2c_client *client;
	struct miscdevice mdev;
	struct power_supply *psy;
	struct power_supply_desc psy_desc;

	struct gpio_desc *mcu_en;
	struct gpio_desc *ap_clk;
	struct gpio_desc *ap_data;
	struct gpio_descs *usb_sw;

	struct pinctrl *pinctrl;
	struct pinctrl_state *pct_default;
	struct pinctrl_state *pct_data_active;
	struct pinctrl_state *pct_data_output;

	int irq;
	bool irq_enabled;
	struct mutex lock;
	struct mutex read_mutex;
	wait_queue_head_t read_wq;

	struct work_struct irq_work;
	struct delayed_work fw_work;
	struct timer_list watchdog;

	u8 *fw_buf;
	size_t fw_len;
	bool fw_allocated;
	bool fw_updated;
	u8 fw_version;
	int mcu_present;

	bool fast_chg_started;
	bool fast_chg_ing;
	bool fast_chg_allow;
	bool fast_low_temp_full;
	bool fast_switch_normal;
	bool fast_normal_warm;
	bool usb_switch_on;
	int last_rx_cmd;
};

static int dash_i2c_write_reg(struct i2c_client *client, u8 reg, const u8 *buf,
			      size_t len)
{
	u8 tmp[DASH_FW_CHUNK_BYTES + 1];
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = 0,
		.len = len + 1,
		.buf = tmp,
	};
	int ret;

	if (len > DASH_FW_CHUNK_BYTES)
		return -EINVAL;

	tmp[0] = reg;
	if (len)
		memcpy(&tmp[1], buf, len);

	ret = i2c_transfer(client->adapter, &msg, 1);
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static int dash_i2c_read_reg(struct i2c_client *client, u8 reg, u8 *buf,
			     size_t len)
{
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &reg,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = buf,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, 2);
	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static void dash_set_usb_switch(struct oneplus_dash *d, bool on)
{
	unsigned long bits = on ? 0x3UL : 0x0UL;

	if (!d->usb_sw)
		return;

	gpiod_set_array_value_cansleep(d->usb_sw->ndescs, d->usb_sw->desc,
				       d->usb_sw->info, &bits);
	d->usb_switch_on = on;
	d->fast_chg_allow = on;
}

static void dash_set_data_input(struct oneplus_dash *d)
{
	gpiod_direction_input(d->ap_data);
	if (d->pinctrl && !IS_ERR_OR_NULL(d->pct_data_active))
		pinctrl_select_state(d->pinctrl, d->pct_data_active);
}

static void dash_set_data_output(struct oneplus_dash *d, int val)
{
	gpiod_direction_output(d->ap_data, val);
	if (d->pinctrl && !IS_ERR_OR_NULL(d->pct_data_output))
		pinctrl_select_state(d->pinctrl, d->pct_data_output);
}

static void dash_mcu_reset_idle(struct oneplus_dash *d)
{
	gpiod_direction_output(d->ap_clk, 0);
	usleep_range(1000, 1200);
	gpiod_direction_output(d->mcu_en, 1);
	usleep_range(10000, 10500);
	gpiod_direction_output(d->mcu_en, 0);
}

static irqreturn_t dash_rx_isr(int irq, void *dev_id)
{
	struct oneplus_dash *d = dev_id;

	schedule_work(&d->irq_work);
	return IRQ_HANDLED;
}

static void dash_arm_irq(struct oneplus_dash *d)
{
	int ret;

	dash_set_data_input(d);
	gpiod_set_value_cansleep(d->ap_clk, 0);
	usleep_range(10000, 10500);
	gpiod_set_value_cansleep(d->ap_clk, 1);

	if (d->irq > 0 && !d->irq_enabled) {
		ret = request_irq(d->irq, dash_rx_isr, IRQF_TRIGGER_RISING,
				  "dash_ap_data", d);
		if (ret)
			dev_err(&d->client->dev, "request irq %d failed: %d\n",
				d->irq, ret);
		else
			d->irq_enabled = true;
	}
}

static void dash_disarm_irq_sync(struct oneplus_dash *d)
{
	if (d->irq > 0 && d->irq_enabled) {
		free_irq(d->irq, d);
		d->irq_enabled = false;
	}
}

static void dash_irq_work(struct work_struct *work)
{
	struct oneplus_dash *d = container_of(work, struct oneplus_dash, irq_work);

	dash_disarm_irq_sync(d);
	msleep(25);
	wake_up_interruptible(&d->read_wq);
}

static void dash_bitbang_write(struct oneplus_dash *d, u8 data)
{
	int i;

	usleep_range(2000, 2200);
	dash_set_data_output(d, 0);

	for (i = 0; i < 3; i++) {
		int bit;

		if (i == 0)
			bit = (data >> 1) & 1;
		else if (i == 1)
			bit = data & 1;
		else
			bit = 0; /* device type 0 = non-bq27541 variant fallback */

		gpiod_set_value_cansleep(d->ap_data, bit);
		gpiod_set_value_cansleep(d->ap_clk, 0);
		usleep_range(1000, 1200);
		gpiod_set_value_cansleep(d->ap_clk, 1);
		usleep_range(19000, 19500);
	}
	dash_set_data_input(d);
}

static int dash_bitbang_read(struct oneplus_dash *d)
{
	int i, val = 0;

	for (i = 0; i < 7; i++) {
		int bit;

		gpiod_set_value_cansleep(d->ap_clk, 0);
		usleep_range(1000, 1200);
		gpiod_set_value_cansleep(d->ap_clk, 1);
		usleep_range(19000, 19500);
		bit = gpiod_get_value_cansleep(d->ap_data) ? 1 : 0;
		val |= bit << (6 - i);
	}
	d->last_rx_cmd = val;
	return val;
}

static int dash_fw_verify(struct oneplus_dash *d)
{
	u8 addr_buf[2] = { 0x88, 0x00 };
	u8 data_buf[32];
	size_t fw_line = 0;
	int i, ret;

	if (!d->fw_buf || d->fw_len < DASH_FW_RECORD_SIZE)
		return -EINVAL;

	ret = dash_i2c_write_reg(d->client, DASH_REG_ADDR, addr_buf, 2);
	if (ret)
		return ret;

	usleep_range(2000, 2200);
	for (i = 0; i < DASH_FW_READ_BLOCKS; i++) {
		u16 addr = DASH_FW_BASE_ADDR + i * 32;
		u16 rec_addr;

		ret = dash_i2c_read_reg(d->client, DASH_REG_READ_DATA,
					&data_buf[0], 16);
		if (ret)
			return ret;
		usleep_range(2000, 2200);
		ret = dash_i2c_read_reg(d->client, DASH_REG_READ_DATA,
					&data_buf[16], 16);
		if (ret)
			return ret;

		if ((fw_line + 1) * DASH_FW_RECORD_SIZE > d->fw_len)
			break;

		rec_addr = d->fw_buf[fw_line * DASH_FW_RECORD_SIZE] |
			   (d->fw_buf[fw_line * DASH_FW_RECORD_SIZE + 1] << 8);
		if (addr == rec_addr) {
			if (memcmp(data_buf,
				   &d->fw_buf[fw_line * DASH_FW_RECORD_SIZE + 2],
				   32) != 0)
				return 1; /* mismatch */
			fw_line++;
		}
	}
	return 0;
}

static int dash_fw_program(struct oneplus_dash *d)
{
	u8 zero = 0, tmp = 0;
	u8 addr_buf[2];
	u16 addr = DASH_FW_BASE_ADDR;
	size_t off = 0;
	int i, ret;

	for (i = 0; i < DASH_FW_ERASE_BLOCKS; i++) {
		addr_buf[0] = addr >> 8;
		addr_buf[1] = addr & 0xff;
		ret = dash_i2c_write_reg(d->client, DASH_REG_ADDR, addr_buf, 2);
		if (ret)
			return ret;
		ret = dash_i2c_write_reg(d->client, DASH_REG_ERASE_BLOCK, &zero, 1);
		if (ret)
			return ret;
		usleep_range(1000, 1200);
		ret = dash_i2c_read_reg(d->client, DASH_REG_ERASE_BLOCK, &tmp, 1);
		if (ret)
			return ret;
		addr += 0x10;
	}

	usleep_range(10000, 10500);
	while (off + DASH_FW_RECORD_SIZE <= d->fw_len) {
		addr_buf[0] = d->fw_buf[off + 1];
		addr_buf[1] = d->fw_buf[off];

		ret = dash_i2c_write_reg(d->client, DASH_REG_ADDR, addr_buf, 2);
		if (ret)
			return ret;

		ret = dash_i2c_write_reg(d->client, DASH_REG_WRITE_DATA,
					 &d->fw_buf[off + 2], 16);
		ret |= dash_i2c_write_reg(d->client, DASH_REG_COMMIT_WRITE, &zero, 1);
		ret |= dash_i2c_read_reg(d->client, DASH_REG_COMMIT_WRITE, &tmp, 1);
		if (ret)
			return ret;

		ret = dash_i2c_write_reg(d->client, DASH_REG_WRITE_DATA,
					 &d->fw_buf[off + 18], 16);
		ret |= dash_i2c_write_reg(d->client, DASH_REG_COMMIT_WRITE, &zero, 1);
		ret |= dash_i2c_read_reg(d->client, DASH_REG_COMMIT_WRITE, &tmp, 1);
		if (ret)
			return ret;

		off += DASH_FW_RECORD_SIZE;
		usleep_range(2000, 2200);
	}

	return 0;
}

static void dash_fw_work(struct work_struct *work)
{
	struct oneplus_dash *d =
		container_of(to_delayed_work(work), struct oneplus_dash, fw_work);
	const struct firmware *fw = NULL;
	u8 zero = 0, tmp = 0;
	int ret, tries;

	if (!d->fw_buf) {
		ret = request_firmware(&fw, DASH_FW_NAME, &d->client->dev);
		if (ret == 0 && fw && fw->size >= DASH_FW_RECORD_SIZE) {
			d->fw_buf = kmemdup(fw->data, fw->size, GFP_KERNEL);
			if (d->fw_buf) {
				d->fw_len = fw->size;
				d->fw_allocated = true;
				d->fw_version = d->fw_buf[d->fw_len - 4];
			}
		}
		if (fw)
			release_firmware(fw);
	}

	if (!d->fw_buf) {
		dev_info(&d->client->dev, "no STM8 firmware loaded yet\n");
		return;
	}

	/* STM8 bootloader initializes I2C (0x26) ~1.85-2.2s after NRST release */
	dash_mcu_reset_idle(d);
	msleep(1800);
	for (tries = 0; tries < 10; tries++) {
		ret = dash_fw_verify(d);
		if (ret >= 0)
			break;
		msleep(100);
	}
	if (ret < 0) {
		d->mcu_present = 0;
		dev_info(&d->client->dev,
			 "STM8 bootloader offline over I2C (%d), keeping safe USB switch off\n",
			 ret);
		return;
	}

	d->mcu_present = 1;
	if (ret == 0) {
		dev_info(&d->client->dev,
			 "STM8 firmware verified (ver 0x%02x, %zu bytes)\n",
			 d->fw_version, d->fw_len);
		goto done_boot;
	}

	dev_info(&d->client->dev, "updating STM8 Dash firmware (%zu bytes)\n",
		 d->fw_len);
	for (tries = 0; tries < 3; tries++) {
		ret = dash_fw_program(d);
		if (ret < 0)
			break;
		ret = dash_fw_verify(d);
		if (ret == 0)
			break;
		gpiod_direction_output(d->mcu_en, 1);
		usleep_range(10000, 10500);
		gpiod_direction_output(d->mcu_en, 0);
		msleep(1000);
	}

	if (ret == 0) {
		dash_i2c_write_reg(d->client, DASH_REG_JUMP_APP, &zero, 1);
		dash_i2c_read_reg(d->client, DASH_REG_JUMP_APP, &tmp, 1);
		dev_info(&d->client->dev, "STM8 firmware updated (ver 0x%02x)\n",
			 d->fw_version);
	} else {
		dev_err(&d->client->dev, "STM8 firmware update failed: %d\n", ret);
	}

done_boot:
	d->fw_updated = true;
	gpiod_direction_output(d->ap_clk, 1);
	usleep_range(10000, 10500);
	gpiod_direction_output(d->mcu_en, 1);
	usleep_range(10000, 10500);
	gpiod_direction_output(d->mcu_en, 0);
	usleep_range(5000, 5500);
	dash_arm_irq(d);
}

static void dash_watchdog_fn(struct timer_list *t)
{
	struct oneplus_dash *d = timer_container_of(d, t, watchdog);

	dev_warn(&d->client->dev, "Dash handshake watchdog expired, reverting USB switch\n");
	d->fast_chg_started = false;
	d->fast_chg_ing = false;
	d->fast_switch_normal = false;
	d->fast_low_temp_full = false;
	d->fast_normal_warm = false;
	dash_set_usb_switch(d, false);
	gpiod_direction_output(d->mcu_en, 0);
	if (d->psy)
		power_supply_changed(d->psy);
}

static bool dash_battery_safe_window(void)
{
	struct power_supply *psy;
	union power_supply_propval soc = {0}, temp = {0};
	int ret;

	psy = power_supply_get_by_name("bq27411-0");
	if (!psy)
		psy = power_supply_get_by_name("battery");
	if (!psy)
		return false;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_CAPACITY, &soc);
	ret |= power_supply_get_property(psy, POWER_SUPPLY_PROP_TEMP, &temp);
	power_supply_put(psy);

	if (ret)
		return false;

	return soc.intval >= DASH_SAFE_SOC_MIN &&
	       soc.intval <= DASH_SAFE_SOC_MAX &&
	       temp.intval >= DASH_SAFE_TEMP_MIN_DECIC &&
	       temp.intval <= DASH_SAFE_TEMP_MAX_DECIC;
}

static int dash_dev_open(struct inode *inode, struct file *filp)
{
	struct oneplus_dash *d =
		container_of(filp->private_data, struct oneplus_dash, mdev);

	filp->private_data = d;
	return 0;
}

static ssize_t dash_dev_read(struct file *filp, char __user *buf, size_t count,
			     loff_t *ppos)
{
	struct oneplus_dash *d = filp->private_data;
	u8 data;
	int ret;

	if (count < 1)
		return -EINVAL;

	mutex_lock(&d->read_mutex);
	ret = wait_event_interruptible(d->read_wq, !d->irq_enabled);
	if (ret) {
		mutex_unlock(&d->read_mutex);
		return ret;
	}
	data = (u8)dash_bitbang_read(d);
	mutex_unlock(&d->read_mutex);

	if (copy_to_user(buf, &data, 1))
		return -EFAULT;

	return 1;
}

static ssize_t dash_dev_write(struct file *filp, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct oneplus_dash *d = filp->private_data;
	u8 *tmp;

	if (count < DASH_FW_RECORD_SIZE || count > 65536)
		return -EINVAL;
	if (d->fw_updated)
		return 0;

	tmp = memdup_user(buf, count);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	mutex_lock(&d->lock);
	if (d->fw_allocated)
		kfree(d->fw_buf);
	d->fw_buf = tmp;
	d->fw_len = count;
	d->fw_allocated = true;
	d->fw_version = d->fw_buf[count - 4];
	mutex_unlock(&d->lock);

	return count;
}

static long dash_dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct oneplus_dash *d = filp->private_data;

	mutex_lock(&d->lock);
	switch (cmd) {
	case DASH_NOTIFY_FIRMWARE_UPDATE:
		schedule_delayed_work(&d->fw_work, msecs_to_jiffies(200));
		break;
	case DASH_NOTIFY_FAST_PRESENT:
		if (arg == DASH_NOTIFY_FAST_PRESENT + 1) {
			d->fast_chg_allow = false;
			d->fast_normal_warm = false;
			mod_timer(&d->watchdog,
				  jiffies + msecs_to_jiffies(DASH_WATCHDOG_MS));
		} else if (arg == DASH_NOTIFY_FAST_PRESENT + 2) {
			dash_bitbang_write(d, DASH_CMD_REJECT);
		} else if (arg == DASH_NOTIFY_FAST_PRESENT + 3) {
			if (dash_battery_safe_window()) {
				dash_bitbang_write(d, DASH_CMD_ALLOW);
				d->fast_chg_started = true;
				d->fast_chg_ing = true;
				dash_set_usb_switch(d, true);
			} else {
				dash_bitbang_write(d, DASH_CMD_REJECT);
			}
		}
		break;
	case DASH_NOTIFY_FAST_ABSENT:
	case DASH_NOTIFY_NORMAL_TEMP_FULL:
	case DASH_NOTIFY_LOW_TEMP_FULL:
	case DASH_NOTIFY_BAD_CONNECTED:
	case DASH_NOTIFY_TEMP_OVER:
	case DASH_NOTIFY_BTB_TEMP_OVER:
	case DASH_NOTIFY_UNDEFINED_CMD:
	case DASH_NOTIFY_INVALID_DATA_CMD:
		timer_delete_sync(&d->watchdog);
		d->fast_chg_started = false;
		d->fast_chg_ing = false;
		dash_set_usb_switch(d, false);
		gpiod_direction_output(d->mcu_en, 0);
		break;
	case DASH_NOTIFY_REQUEST_IRQ:
		dash_arm_irq(d);
		break;
	case DASH_NOTIFY_ALLOW_READING_IIC:
	case DASH_NOTIFY_UPDATE_DASH_PRESENT:
	case DASH_NOTIFY_ADAPTER_FW_UPDATE:
		break;
	default:
		mutex_unlock(&d->lock);
		return -EINVAL;
	}
	if (d->psy)
		power_supply_changed(d->psy);
	mutex_unlock(&d->lock);
	return 0;
}

static const struct file_operations dash_fops = {
	.owner = THIS_MODULE,
	.llseek = noop_llseek,
	.open = dash_dev_open,
	.read = dash_dev_read,
	.write = dash_dev_write,
	.unlocked_ioctl = dash_dev_ioctl,
	.compat_ioctl = dash_dev_ioctl,
};

static enum power_supply_property dash_psy_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
};

static int dash_psy_get_prop(struct power_supply *psy,
			     enum power_supply_property psp,
			     union power_supply_propval *val)
{
	struct oneplus_dash *d = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = d->fast_chg_started ? 1 : 0;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = d->fast_chg_ing ? POWER_SUPPLY_STATUS_CHARGING :
			      POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = d->fast_chg_ing ? POWER_SUPPLY_CHARGE_TYPE_FAST :
			      POWER_SUPPLY_CHARGE_TYPE_NONE;
		return 0;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = d->fast_chg_ing ? 4000000 : 0;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = 5000000;
		return 0;
	default:
		return -EINVAL;
	}
}

static ssize_t fast_chg_started_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct oneplus_dash *d = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", d->fast_chg_started ? 1 : 0);
}
static DEVICE_ATTR_RO(fast_chg_started);

static ssize_t fw_version_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct oneplus_dash *d = dev_get_drvdata(dev);
	return sysfs_emit(buf, "0x%02x\n", d->fw_version);
}
static DEVICE_ATTR_RO(fw_version);

static ssize_t mcu_present_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct oneplus_dash *d = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", d->mcu_present);
}
static DEVICE_ATTR_RO(mcu_present);

static ssize_t usb_switch_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct oneplus_dash *d = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", d->usb_switch_on ? 1 : 0);
}
static DEVICE_ATTR_RO(usb_switch);

static struct attribute *dash_attrs[] = {
	&dev_attr_fast_chg_started.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_mcu_present.attr,
	&dev_attr_usb_switch.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dash);

static int oneplus_dash_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct oneplus_dash *d;
	struct power_supply_config psy_cfg = {};
	int ret;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->client = client;
	i2c_set_clientdata(client, d);
	mutex_init(&d->lock);
	mutex_init(&d->read_mutex);
	init_waitqueue_head(&d->read_wq);
	INIT_WORK(&d->irq_work, dash_irq_work);
	INIT_DELAYED_WORK(&d->fw_work, dash_fw_work);
	timer_setup(&d->watchdog, dash_watchdog_fn, 0);

	d->mcu_en = devm_gpiod_get(dev, "mcu-en", GPIOD_OUT_LOW);
	if (IS_ERR(d->mcu_en))
		return dev_err_probe(dev, PTR_ERR(d->mcu_en), "mcu-en gpio\n");

	d->ap_clk = devm_gpiod_get(dev, "ap-clk", GPIOD_OUT_LOW);
	if (IS_ERR(d->ap_clk))
		return dev_err_probe(dev, PTR_ERR(d->ap_clk), "ap-clk gpio\n");

	d->ap_data = devm_gpiod_get(dev, "ap-data", GPIOD_IN);
	if (IS_ERR(d->ap_data))
		return dev_err_probe(dev, PTR_ERR(d->ap_data), "ap-data gpio\n");

	d->usb_sw = devm_gpiod_get_array(dev, "usb-sw", GPIOD_OUT_LOW);
	if (IS_ERR(d->usb_sw))
		return dev_err_probe(dev, PTR_ERR(d->usb_sw), "usb-sw gpios\n");

	/* Always ensure USB D+/D- routes to MSM8998 PHY by default. */
	dash_set_usb_switch(d, false);

	d->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(d->pinctrl)) {
		d->pct_default = pinctrl_lookup_state(d->pinctrl, "default");
		d->pct_data_active = pinctrl_lookup_state(d->pinctrl, "data_active");
		d->pct_data_output = pinctrl_lookup_state(d->pinctrl, "data_output");
		if (!IS_ERR_OR_NULL(d->pct_default))
			pinctrl_select_state(d->pinctrl, d->pct_default);
		if (!IS_ERR_OR_NULL(d->pct_data_active))
			pinctrl_select_state(d->pinctrl, d->pct_data_active);
	} else {
		d->pinctrl = NULL;
	}

	d->irq = gpiod_to_irq(d->ap_data);

	d->mdev.minor = MISC_DYNAMIC_MINOR;
	d->mdev.name = "dash";
	d->mdev.fops = &dash_fops;
	d->mdev.parent = dev;
	ret = misc_register(&d->mdev);
	if (ret)
		return ret;

	d->psy_desc.name = "oneplus-dash";
	d->psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
	d->psy_desc.properties = dash_psy_props;
	d->psy_desc.num_properties = ARRAY_SIZE(dash_psy_props);
	d->psy_desc.get_property = dash_psy_get_prop;
	psy_cfg.drv_data = d;
	psy_cfg.fwnode = dev_fwnode(dev);

	d->psy = devm_power_supply_register(dev, &d->psy_desc, &psy_cfg);
	if (IS_ERR(d->psy)) {
		misc_deregister(&d->mdev);
		return PTR_ERR(d->psy);
	}

	dash_mcu_reset_idle(d);
	schedule_delayed_work(&d->fw_work, msecs_to_jiffies(1000));

	dev_info(dev, "OnePlus Dash MCU registered (irq=%d)\n", d->irq);
	return 0;
}

static void oneplus_dash_remove(struct i2c_client *client)
{
	struct oneplus_dash *d = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&d->fw_work);
	timer_delete_sync(&d->watchdog);
	dash_disarm_irq_sync(d);
	cancel_work_sync(&d->irq_work);
	dash_set_usb_switch(d, false);
	gpiod_direction_output(d->mcu_en, 1);
	misc_deregister(&d->mdev);
	if (d->fw_allocated)
		kfree(d->fw_buf);
}

static void oneplus_dash_shutdown(struct i2c_client *client)
{
	struct oneplus_dash *d = i2c_get_clientdata(client);

	dash_set_usb_switch(d, false);
	gpiod_direction_output(d->mcu_en, 1);
}

static const struct of_device_id oneplus_dash_of_match[] = {
	{ .compatible = "oneplus,dash-charger" },
	{ .compatible = "microchip,oneplus_fastcg" },
	{ }
};
MODULE_DEVICE_TABLE(of, oneplus_dash_of_match);

static const struct i2c_device_id oneplus_dash_id[] = {
	{ "oneplus-dash", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, oneplus_dash_id);

static struct i2c_driver oneplus_dash_driver = {
	.driver = {
		.name = "oneplus-dash",
		.of_match_table = oneplus_dash_of_match,
		.dev_groups = dash_groups,
	},
	.probe = oneplus_dash_probe,
	.remove = oneplus_dash_remove,
	.shutdown = oneplus_dash_shutdown,
	.id_table = oneplus_dash_id,
};
module_i2c_driver(oneplus_dash_driver);

MODULE_DESCRIPTION("Clean-room OnePlus 5T Dash/VOOC STM8 MCU Driver");
MODULE_LICENSE("GPL");
