// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clean-room Goodix GF5216/GF5288 Fingerprint & ID Detect Driver for OnePlus 5T
 * Hardware contract derived from stock-oneplus-msm8998 (goodix + fpdetect)
 * and stock_02.dts (GPIOs 25, 39, 41, 63, 65, 75, 121; Netlink 25; /dev/goodix_fp).
 */

#define pr_fmt(fmt) "goodix_fp: " fmt

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netlink.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/skbuff.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <net/sock.h>
#include <uapi/linux/goodix_fp.h>

#define GF_DEV_NAME		"goodix_fp"
#define GF_INPUT_NAME		"gf_input"
#define GF_NL_MAX_MSGSIZE	32
#define GF_WAKELOCK_HOLD_MS	500

#define FP_VER_UNPROBED		(-1)
#define FP_VER_FPC1245		0x01
#define FP_VER_FPC1263		0x02
#define FP_VER_GOODIX		0x03

static atomic_t fp_detect_version = ATOMIC_INIT(FP_VER_UNPROBED);
static char fp_detect_name[32] = "unknown";

/* ---------- Fingerprint Sensor ID Detector ("oneplus,fpdetect") ---------- */

struct fpdetect_data {
	struct device *dev;
	struct gpio_desc *en_gpio;
	struct gpio_desc *id_gpios[3];
	int sensor_version;
};

static ssize_t sensor_version_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct fpdetect_data *fd = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%02x (%s)\n", fd->sensor_version, fp_detect_name);
}
static DEVICE_ATTR_RO(sensor_version);

static struct attribute *fpdetect_attrs[] = {
	&dev_attr_sensor_version.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fpdetect);

static int fpdetect_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fpdetect_data *fd;
	int id0, id1, id2;

	fd = devm_kzalloc(dev, sizeof(*fd), GFP_KERNEL);
	if (!fd)
		return -ENOMEM;

	fd->dev = dev;
	fd->en_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(fd->en_gpio))
		return dev_err_probe(dev, PTR_ERR(fd->en_gpio),
				     "failed to get enable gpio\n");

	fd->id_gpios[0] = devm_gpiod_get(dev, "id0", GPIOD_IN);
	if (IS_ERR(fd->id_gpios[0]))
		return PTR_ERR(fd->id_gpios[0]);
	fd->id_gpios[1] = devm_gpiod_get(dev, "id1", GPIOD_IN);
	if (IS_ERR(fd->id_gpios[1]))
		return PTR_ERR(fd->id_gpios[1]);
	fd->id_gpios[2] = devm_gpiod_get(dev, "id2", GPIOD_IN);
	if (IS_ERR(fd->id_gpios[2]))
		return PTR_ERR(fd->id_gpios[2]);

	msleep(2);
	id0 = gpiod_get_value_cansleep(fd->id_gpios[0]);
	id1 = gpiod_get_value_cansleep(fd->id_gpios[1]);
	id2 = gpiod_get_value_cansleep(fd->id_gpios[2]);

	/* OnePlus 5T (dumpling) ID matrix per stock fingerprint_detect.c */
	if (id0 && !id1 && id2) {
		fd->sensor_version = FP_VER_GOODIX;
		strscpy(fp_detect_name, "gfp5288", sizeof(fp_detect_name));
	} else if (!id0 && id1 && id2) {
		fd->sensor_version = FP_VER_GOODIX;
		strscpy(fp_detect_name, "gfp5288", sizeof(fp_detect_name));
	} else if (id0 && id1 && !id2) {
		fd->sensor_version = FP_VER_GOODIX;
		strscpy(fp_detect_name, "goodix3268", sizeof(fp_detect_name));
	} else {
		fd->sensor_version = FP_VER_GOODIX;
		strscpy(fp_detect_name, "goodix-fallback", sizeof(fp_detect_name));
		dev_warn(dev, "unexpected FP ID pins (%d,%d,%d); assuming dumpling Goodix\n",
			 id0, id1, id2);
	}

	platform_set_drvdata(pdev, fd);
	atomic_set(&fp_detect_version, fd->sensor_version);
	dev_info(dev, "detected FP sensor id=(%d,%d,%d) model=%s version=0x%02x\n",
		 id0, id1, id2, fp_detect_name, fd->sensor_version);
	return 0;
}

static const struct of_device_id fpdetect_of_match[] = {
	{ .compatible = "oneplus,fpdetect" },
	{ }
};
MODULE_DEVICE_TABLE(of, fpdetect_of_match);

static struct platform_driver fpdetect_driver = {
	.probe = fpdetect_probe,
	.driver = {
		.name = "fingerprint_detect",
		.of_match_table = fpdetect_of_match,
		.dev_groups = fpdetect_groups,
	},
};

/* ---------- Goodix GF5216/GF5288 Platform + Netlink + Miscdev ---------- */

struct gf_op5t_dev {
	struct device *dev;
	struct miscdevice misc;
	struct input_dev *input;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pin_en;
	struct pinctrl_state *pin_dis;
	struct gpio_desc *en_gpio;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *irq_gpio;
	int irq;
	struct mutex lock;
	bool irq_enabled;
	bool powered;
	u32 users;
	struct wakeup_source *ws;
	struct sock *nl_sk;
	atomic_t nl_pid;
	struct notifier_block pm_nb;
};

static struct gf_op5t_dev *g_gf;

static const struct {
	unsigned int type;
	unsigned int code;
} gf_key_maps[] = {
	{ EV_KEY, KEY_HOME },
	{ EV_KEY, KEY_MENU },
	{ EV_KEY, KEY_BACK },
	{ EV_KEY, KEY_POWER },
	{ EV_KEY, KEY_CAMERA },
	{ EV_KEY, KEY_UP },
	{ EV_KEY, KEY_DOWN },
	{ EV_KEY, KEY_LEFT },
	{ EV_KEY, KEY_RIGHT },
	{ EV_KEY, KEY_VOLUMEDOWN },
	{ EV_KEY, KEY_VOLUMEUP },
	{ EV_KEY, BTN_B },
	{ EV_KEY, KEY_F2 },
	{ EV_KEY, KEY_CHAT },
};

static void gf_send_nl_event(struct gf_op5t_dev *gf, u8 event)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	u32 pid;
	char msg[GF_NL_MAX_MSGSIZE] = { (char)event, 0 };

	if (!gf || !gf->nl_sk)
		return;
	pid = (u32)atomic_read(&gf->nl_pid);
	if (!pid)
		return;

	skb = nlmsg_new(GF_NL_MAX_MSGSIZE, GFP_KERNEL);
	if (!skb)
		return;

	nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, GF_NL_MAX_MSGSIZE, 0);
	if (!nlh) {
		kfree_skb(skb);
		return;
	}
	NETLINK_CB(skb).dst_group = 0;
	memcpy(nlmsg_data(nlh), msg, sizeof(msg));
	netlink_unicast(gf->nl_sk, skb, pid, MSG_DONTWAIT);
}

static void gf_nl_recv(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;

	if (!g_gf || !skb || skb->len < nlmsg_total_size(0))
		return;
	nlh = nlmsg_hdr(skb);
	if (nlh->nlmsg_pid)
		atomic_set(&g_gf->nl_pid, (int)nlh->nlmsg_pid);
}

static void gf_enable_irq_locked(struct gf_op5t_dev *gf)
{
	if (!gf->irq_enabled) {
		enable_irq(gf->irq);
		gf->irq_enabled = true;
	}
}

static void gf_disable_irq_locked(struct gf_op5t_dev *gf)
{
	if (gf->irq_enabled) {
		disable_irq(gf->irq);
		gf->irq_enabled = false;
	}
}

static void gf_hw_reset(struct gf_op5t_dev *gf, unsigned int delay_ms)
{
	gpiod_set_value_cansleep(gf->reset_gpio, 0);
	mdelay(3);
	gpiod_set_value_cansleep(gf->reset_gpio, 1);
	if (delay_ms)
		mdelay(delay_ms);
}

static void gf_set_power_locked(struct gf_op5t_dev *gf, bool on)
{
	if (gf->powered == on)
		return;

	if (on) {
		if (gf->pinctrl && gf->pin_en)
			pinctrl_select_state(gf->pinctrl, gf->pin_en);
		gpiod_set_value_cansleep(gf->en_gpio, 1);
		msleep(10);
	} else {
		gpiod_set_value_cansleep(gf->en_gpio, 0);
		if (gf->pinctrl && gf->pin_dis)
			pinctrl_select_state(gf->pinctrl, gf->pin_dis);
	}
	gf->powered = on;
}

static void gf_report_nav(struct gf_op5t_dev *gf, gf_nav_event_t nav)
{
	unsigned int code;

	switch (nav) {
	case GF_NAV_UP:
		code = KEY_UP;
		break;
	case GF_NAV_DOWN:
		code = KEY_DOWN;
		break;
	case GF_NAV_LEFT:
		code = KEY_LEFT;
		break;
	case GF_NAV_RIGHT:
		code = KEY_RIGHT;
		break;
	case GF_NAV_CLICK:
		code = KEY_VOLUMEDOWN;
		break;
	case GF_NAV_DOUBLE_CLICK:
		code = KEY_VOLUMEUP;
		break;
	case GF_NAV_LONG_PRESS:
		code = BTN_B;
		break;
	case GF_NAV_HEAVY:
		code = KEY_CHAT;
		break;
	case GF_NAV_F2:
		code = KEY_F2;
		break;
	default:
		return;
	}

	input_report_key(gf->input, code, 1);
	input_sync(gf->input);
	input_report_key(gf->input, code, 0);
	input_sync(gf->input);
}

static void gf_report_key(struct gf_op5t_dev *gf, const struct gf_key *k)
{
	unsigned int code;

	switch (k->key) {
	case GF_KEY_HOME:
		code = KEY_HOME;
		input_report_key(gf->input, code, !!k->value);
		input_sync(gf->input);
		return;
	case GF_KEY_POWER:
		code = KEY_POWER;
		break;
	case GF_KEY_CAMERA:
		code = KEY_CAMERA;
		break;
	case GF_KEY_MENU:
		code = KEY_MENU;
		break;
	case GF_KEY_BACK:
		code = KEY_BACK;
		break;
	default:
		code = k->key;
		break;
	}

	if (k->value == 1) {
		input_report_key(gf->input, code, 1);
		input_sync(gf->input);
		input_report_key(gf->input, code, 0);
		input_sync(gf->input);
	}
}

static irqreturn_t gf_irq_thread(int irq, void *data)
{
	struct gf_op5t_dev *gf = data;

	__pm_wakeup_event(gf->ws, GF_WAKELOCK_HOLD_MS);
	gf_send_nl_event(gf, GF_NET_EVENT_IRQ);
	return IRQ_HANDLED;
}

static int gf_pm_notify(struct notifier_block *nb, unsigned long mode, void *unused)
{
	struct gf_op5t_dev *gf = container_of(nb, struct gf_op5t_dev, pm_nb);

	if (!gf->powered)
		return NOTIFY_DONE;

	if (mode == PM_SUSPEND_PREPARE)
		gf_send_nl_event(gf, GF_NET_EVENT_FB_BLACK);
	else if (mode == PM_POST_SUSPEND)
		gf_send_nl_event(gf, GF_NET_EVENT_FB_UNBLACK);

	return NOTIFY_DONE;
}

static int gf_open(struct inode *inode, struct file *filp)
{
	struct gf_op5t_dev *gf = g_gf;

	if (!gf)
		return -ENODEV;

	mutex_lock(&gf->lock);
	filp->private_data = gf;
	nonseekable_open(inode, filp);
	gf->users++;
	gf_set_power_locked(gf, true);
	if (gf->users == 1)
		gf_enable_irq_locked(gf);
	gf_hw_reset(gf, 3);
	mutex_unlock(&gf->lock);
	return 0;
}

static int gf_release(struct inode *inode, struct file *filp)
{
	struct gf_op5t_dev *gf = filp->private_data;

	if (!gf)
		return 0;

	mutex_lock(&gf->lock);
	if (gf->users > 0 && --gf->users == 0) {
		gf_disable_irq_locked(gf);
		gf_set_power_locked(gf, false);
	}
	mutex_unlock(&gf->lock);
	return 0;
}

static long gf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct gf_op5t_dev *gf = filp->private_data;
	void __user *argp = (void __user *)arg;
	struct gf_key k;
	gf_nav_event_t nav;
	struct gf_ioc_chip_info info;
	u8 nl_proto = NETLINK_GOODIX;
	int ret = 0;

	if (!gf || _IOC_TYPE(cmd) != GF_IOC_MAGIC)
		return -ENODEV;

	if ((_IOC_DIR(cmd) & (_IOC_READ | _IOC_WRITE)) &&
	    !access_ok(argp, _IOC_SIZE(cmd)))
		return -EFAULT;

	mutex_lock(&gf->lock);
	if (!gf->powered && cmd != GF_IOC_ENABLE_POWER &&
	    cmd != GF_IOC_DISABLE_POWER && cmd != GF_IOC_INIT) {
		mutex_unlock(&gf->lock);
		return -ENODEV;
	}

	switch (cmd) {
	case GF_IOC_INIT:
		if (copy_to_user(argp, &nl_proto, sizeof(nl_proto)))
			ret = -EFAULT;
		break;
	case GF_IOC_EXIT:
	case GF_IOC_ENTER_SLEEP_MODE:
	case GF_IOC_GET_FW_INFO:
	case GF_IOC_REMOVE:
	case GF_IOC_ENABLE_SPI_CLK:
	case GF_IOC_DISABLE_SPI_CLK:
		break;
	case GF_IOC_ENABLE_IRQ:
		gf_enable_irq_locked(gf);
		break;
	case GF_IOC_DISABLE_IRQ:
		gf_disable_irq_locked(gf);
		break;
	case GF_IOC_RESET:
		gf_hw_reset(gf, 3);
		break;
	case GF_IOC_ENABLE_POWER:
		gf_set_power_locked(gf, true);
		break;
	case GF_IOC_DISABLE_POWER:
		gf_set_power_locked(gf, false);
		break;
	case GF_IOC_INPUT_KEY_EVENT:
		if (copy_from_user(&k, argp, sizeof(k))) {
			ret = -EFAULT;
			break;
		}
		gf_report_key(gf, &k);
		break;
	case GF_IOC_NAV_EVENT:
		if (copy_from_user(&nav, argp, sizeof(nav))) {
			ret = -EFAULT;
			break;
		}
		gf_report_nav(gf, nav);
		break;
	case GF_IOC_CHIP_INFO:
		if (copy_from_user(&info, argp, sizeof(info))) {
			ret = -EFAULT;
			break;
		}
		dev_info(gf->dev, "chip vendor=0x%x mode=0x%x op=0x%x\n",
			 info.vendor_id, info.mode, info.operation);
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&gf->lock);
	return ret;
}

static const struct file_operations gf_fops = {
	.owner = THIS_MODULE,
	.open = gf_open,
	.release = gf_release,
	.unlocked_ioctl = gf_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int gf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *detect_np;
	struct gf_op5t_dev *gf;
	struct netlink_kernel_cfg nl_cfg = {
		.input = gf_nl_recv,
	};
	int ret, i;

	detect_np = of_find_compatible_node(NULL, NULL, "oneplus,fpdetect");
	if (detect_np) {
		of_node_put(detect_np);
		if (atomic_read(&fp_detect_version) == FP_VER_UNPROBED)
			return -EPROBE_DEFER;
		if (atomic_read(&fp_detect_version) != FP_VER_GOODIX)
			return -ENODEV;
	}

	gf = devm_kzalloc(dev, sizeof(*gf), GFP_KERNEL);
	if (!gf)
		return -ENOMEM;

	gf->dev = dev;
	mutex_init(&gf->lock);
	atomic_set(&gf->nl_pid, 0);

	gf->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(gf->pinctrl)) {
		gf->pin_en = pinctrl_lookup_state(gf->pinctrl, "fp_en_init");
		if (IS_ERR(gf->pin_en))
			gf->pin_en = NULL;
		gf->pin_dis = pinctrl_lookup_state(gf->pinctrl, "fp_dis_init");
		if (IS_ERR(gf->pin_dis))
			gf->pin_dis = NULL;
		if (gf->pin_en)
			pinctrl_select_state(gf->pinctrl, gf->pin_en);
	} else {
		gf->pinctrl = NULL;
	}

	gf->en_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(gf->en_gpio))
		return dev_err_probe(dev, PTR_ERR(gf->en_gpio),
				     "failed to get enable gpio\n");

	gf->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(gf->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(gf->reset_gpio),
				     "failed to get reset gpio\n");

	gf->irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(gf->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(gf->irq_gpio),
				     "failed to get irq gpio\n");

	gf->irq = gpiod_to_irq(gf->irq_gpio);
	if (gf->irq < 0)
		return gf->irq;

	gf->input = devm_input_allocate_device(dev);
	if (!gf->input)
		return -ENOMEM;
	gf->input->name = GF_INPUT_NAME;
	for (i = 0; i < ARRAY_SIZE(gf_key_maps); i++)
		input_set_capability(gf->input, gf_key_maps[i].type,
				     gf_key_maps[i].code);
	ret = input_register_device(gf->input);
	if (ret)
		return ret;

	gf->ws = wakeup_source_register(dev, "fp_wakelock");
	if (!gf->ws)
		return -ENOMEM;

	gf->nl_sk = netlink_kernel_create(&init_net, NETLINK_GOODIX, &nl_cfg);
	if (!gf->nl_sk) {
		ret = -ENOMEM;
		goto err_ws;
	}

	g_gf = gf;
	ret = devm_request_threaded_irq(dev, gf->irq, NULL, gf_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"gf_fp", gf);
	if (ret)
		goto err_nl;

	disable_irq(gf->irq);
	gf->irq_enabled = false;
	enable_irq_wake(gf->irq);

	gf->pm_nb.notifier_call = gf_pm_notify;
	register_pm_notifier(&gf->pm_nb);

	gf->misc.minor = MISC_DYNAMIC_MINOR;
	gf->misc.name = GF_DEV_NAME;
	gf->misc.fops = &gf_fops;
	ret = misc_register(&gf->misc);
	if (ret)
		goto err_pm;

	platform_set_drvdata(pdev, gf);
	dev_info(dev, "Goodix FP initialized (/dev/%s, irq=%d, nl=%d)\n",
		 GF_DEV_NAME, gf->irq, NETLINK_GOODIX);
	return 0;

err_pm:
	unregister_pm_notifier(&gf->pm_nb);
	disable_irq_wake(gf->irq);
err_nl:
	g_gf = NULL;
	netlink_kernel_release(gf->nl_sk);
err_ws:
	wakeup_source_unregister(gf->ws);
	return ret;
}

static void gf_remove(struct platform_device *pdev)
{
	struct gf_op5t_dev *gf = platform_get_drvdata(pdev);

	misc_deregister(&gf->misc);
	unregister_pm_notifier(&gf->pm_nb);
	disable_irq_wake(gf->irq);
	if (gf->irq_enabled)
		disable_irq(gf->irq);
	g_gf = NULL;
	netlink_kernel_release(gf->nl_sk);
	wakeup_source_unregister(gf->ws);
}

static const struct of_device_id gf_of_match[] = {
	{ .compatible = "goodix,fingerprint" },
	{ }
};
MODULE_DEVICE_TABLE(of, gf_of_match);

static struct platform_driver gf_driver = {
	.probe = gf_probe,
	.remove = gf_remove,
	.driver = {
		.name = GF_DEV_NAME,
		.of_match_table = gf_of_match,
	},
};

static int __init goodix_op5t_init(void)
{
	int ret;

	ret = platform_driver_register(&fpdetect_driver);
	if (ret)
		return ret;
	ret = platform_driver_register(&gf_driver);
	if (ret) {
		platform_driver_unregister(&fpdetect_driver);
		return ret;
	}
	return 0;
}

static void __exit goodix_op5t_exit(void)
{
	platform_driver_unregister(&gf_driver);
	platform_driver_unregister(&fpdetect_driver);
}

module_init(goodix_op5t_init);
module_exit(goodix_op5t_exit);

MODULE_DESCRIPTION("Clean-room Goodix GF5216/GF5288 Fingerprint Driver for OnePlus 5T");
MODULE_LICENSE("GPL");
