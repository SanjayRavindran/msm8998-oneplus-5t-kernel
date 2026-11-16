// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clean-room NXP P61 / P73 SPI embedded Secure Element (eSE) driver
 * for OnePlus 5T (dumpling / MSM8998 BLSP1 SPI1).
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <uapi/linux/p61_ese.h>

#define P61_MAX_BUF_SIZE	1024
#define P61_DEFAULT_SPI_HZ	8000000

struct p61_ese {
	struct spi_device *spi;
	struct miscdevice mdev;
	struct gpio_desc *ese_pwr;
	struct gpio_desc *reset;
	struct mutex read_lock;
	struct mutex write_lock;
	struct mutex pwr_lock;
	bool pwr_enabled;
	bool poll_mode;
};

static int p61_set_power(struct p61_ese *ese, bool on)
{
	mutex_lock(&ese->pwr_lock);
	if (ese->ese_pwr) {
		gpiod_set_value_cansleep(ese->ese_pwr, on ? 1 : 0);
		usleep_range(on ? 10000 : 1000, on ? 12000 : 1500);
	}
	ese->pwr_enabled = on;
	mutex_unlock(&ese->pwr_lock);
	return 0;
}

static int p61_dev_open(struct inode *inode, struct file *filp)
{
	struct p61_ese *ese =
		container_of(filp->private_data, struct p61_ese, mdev);

	filp->private_data = ese;
	return 0;
}

static ssize_t p61_dev_read(struct file *filp, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct p61_ese *ese = filp->private_data;
	u8 *rx;
	int ret;

	if (!count)
		return 0;
	if (count > P61_MAX_BUF_SIZE)
		count = P61_MAX_BUF_SIZE;

	rx = kzalloc(count, GFP_KERNEL);
	if (!rx)
		return -ENOMEM;

	mutex_lock(&ese->read_lock);
	ret = spi_read(ese->spi, rx, count);
	mutex_unlock(&ese->read_lock);

	if (ret < 0)
		goto out;

	if (copy_to_user(buf, rx, count)) {
		ret = -EFAULT;
		goto out;
	}
	ret = count;
out:
	kfree(rx);
	return ret;
}

static ssize_t p61_dev_write(struct file *filp, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct p61_ese *ese = filp->private_data;
	u8 *tx;
	int ret;

	if (!count)
		return 0;
	if (count > P61_MAX_BUF_SIZE)
		count = P61_MAX_BUF_SIZE;

	tx = memdup_user(buf, count);
	if (IS_ERR(tx))
		return PTR_ERR(tx);

	mutex_lock(&ese->write_lock);
	ret = spi_write(ese->spi, tx, count);
	mutex_unlock(&ese->write_lock);

	kfree(tx);
	return ret < 0 ? ret : count;
}

static long p61_dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct p61_ese *ese = filp->private_data;
	long state;

	switch (cmd) {
	case P61_SET_PWR:
	case P61_SET_SPM_PWR:
		if (arg == 0 || arg == 1)
			return p61_set_power(ese, arg != 0);
		if (arg == 2 && ese->reset) {
			gpiod_set_value_cansleep(ese->reset, 0);
			msleep(20);
			gpiod_set_value_cansleep(ese->reset, 1);
			msleep(20);
			return 0;
		}
		return 0;
	case P61_GET_SPM_STATUS:
		mutex_lock(&ese->pwr_lock);
		state = ese->pwr_enabled ? 1 : 0;
		mutex_unlock(&ese->pwr_lock);
		return state;
	case P61_SET_POLL:
		ese->poll_mode = (arg != 0);
		return 0;
	case P61_SET_DBG:
	case P61_SET_THROUGHPUT:
	case P61_GET_ESE_ACCESS:
	case P61_SET_POWER_SCHEME:
	case P61_SET_DWNLD_STATUS:
	case P61_INHIBIT_PWR_CNTRL:
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct file_operations p61_fops = {
	.owner = THIS_MODULE,
	.open = p61_dev_open,
	.read = p61_dev_read,
	.write = p61_dev_write,
	.unlocked_ioctl = p61_dev_ioctl,
	.compat_ioctl = p61_dev_ioctl,
};

static ssize_t power_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct p61_ese *ese = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", ese->pwr_enabled ? 1 : 0);
}

static ssize_t power_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct p61_ese *ese = dev_get_drvdata(dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;
	p61_set_power(ese, val);
	return count;
}
static DEVICE_ATTR_RW(power);

static struct attribute *p61_attrs[] = {
	&dev_attr_power.attr,
	NULL,
};
ATTRIBUTE_GROUPS(p61);

static int nxp_p61_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct p61_ese *ese;
	int ret;

	ese = devm_kzalloc(dev, sizeof(*ese), GFP_KERNEL);
	if (!ese)
		return -ENOMEM;

	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0;
	if (!spi->max_speed_hz)
		spi->max_speed_hz = P61_DEFAULT_SPI_HZ;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "spi_setup failed\n");

	ese->ese_pwr = devm_gpiod_get_optional(dev, "ese-pwr", GPIOD_OUT_LOW);
	if (IS_ERR(ese->ese_pwr))
		return dev_err_probe(dev, PTR_ERR(ese->ese_pwr), "ese-pwr gpio\n");

	ese->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ese->reset))
		return dev_err_probe(dev, PTR_ERR(ese->reset), "reset gpio\n");

	ese->spi = spi;
	mutex_init(&ese->read_lock);
	mutex_init(&ese->write_lock);
	mutex_init(&ese->pwr_lock);
	spi_set_drvdata(spi, ese);

	ese->mdev.minor = MISC_DYNAMIC_MINOR;
	ese->mdev.name = "p73";
	ese->mdev.fops = &p61_fops;
	ese->mdev.parent = dev;
	ret = misc_register(&ese->mdev);
	if (ret)
		return ret;

	dev_info(dev, "NXP P61 SPI eSE registered (/dev/p73, %u Hz)\n",
		 spi->max_speed_hz);
	return 0;
}

static void nxp_p61_remove(struct spi_device *spi)
{
	struct p61_ese *ese = spi_get_drvdata(spi);

	misc_deregister(&ese->mdev);
	p61_set_power(ese, false);
}

static const struct of_device_id nxp_p61_of_match[] = {
	{ .compatible = "nxp,p61" },
	{ .compatible = "nxp,p73" },
	{ }
};
MODULE_DEVICE_TABLE(of, nxp_p61_of_match);

static const struct spi_device_id nxp_p61_id[] = {
	{ "p61", 0 },
	{ "p73", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, nxp_p61_id);

static struct spi_driver nxp_p61_driver = {
	.driver = {
		.name = "nxp-p61-ese",
		.of_match_table = nxp_p61_of_match,
		.dev_groups = p61_groups,
	},
	.probe = nxp_p61_probe,
	.remove = nxp_p61_remove,
	.id_table = nxp_p61_id,
};
module_spi_driver(nxp_p61_driver);

MODULE_DESCRIPTION("Clean-room NXP P61/P73 SPI eSE Driver");
MODULE_LICENSE("GPL");
