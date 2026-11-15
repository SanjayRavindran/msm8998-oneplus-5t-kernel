// SPDX-License-Identifier: GPL-2.0
/*
 * Clean-room V4L2 lens actuator driver for ON Semiconductor LC898217XC
 * used on OnePlus 5T (dumpling) rear cameras (IMX398 @ 0x72, IMX376K @ 0x74).
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-subdev.h>

#define LC898217XC_REG_DAC		0x84
#define LC898217XC_REG_INIT		0xb3
#define LC898217XC_INIT_ACTIVE		0x01
#define LC898217XC_REG_STATUS		0xf0

#define LC898217XC_INIT_DELAY_US	10000
#define LC898217XC_SUSPEND_DELAY_MS	1000

/*
 * Expose standard V4L2 focus range [0..1023] centered at 512 and map linearly
 * onto the closed-loop signed 12-bit DAC range [-384..+384].
 */
#define LC898217XC_FOCUS_MAX		1023
#define LC898217XC_FOCUS_DEFAULT	512
#define LC898217XC_DAC_ABS_MAX		384

static const char * const lc898217xc_supply_names[] = {
	"vio",
	"vaf",
};

struct lc898217xc_dev {
	struct device *dev;
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrls;
	struct regulator_bulk_data supplies[ARRAY_SIZE(lc898217xc_supply_names)];
	struct mutex lock;
	s32 focus_pos;
};

static inline struct lc898217xc_dev *to_lc898217xc(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct lc898217xc_dev, ctrls);
}

static inline struct lc898217xc_dev *sd_to_lc898217xc(struct v4l2_subdev *sd)
{
	return container_of(sd, struct lc898217xc_dev, sd);
}

static int lc898217xc_write_u8(struct i2c_client *client, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	int ret;

	ret = i2c_master_send(client, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	return ret == sizeof(buf) ? 0 : -EIO;
}

static int lc898217xc_write_u16(struct i2c_client *client, u8 reg, u16 val)
{
	u8 buf[3];
	int ret;

	buf[0] = reg;
	put_unaligned_be16(val, &buf[1]);
	ret = i2c_master_send(client, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	return ret == sizeof(buf) ? 0 : -EIO;
}

static int lc898217xc_read_bytes(struct i2c_client *client, u8 reg,
				 u8 *val, size_t len)
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
			.buf = val,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

static s16 lc898217xc_focus_to_dac(s32 focus)
{
	s32 scaled = ((focus - LC898217XC_FOCUS_DEFAULT) * LC898217XC_DAC_ABS_MAX) /
		     LC898217XC_FOCUS_DEFAULT;

	return clamp_t(s32, scaled, -LC898217XC_DAC_ABS_MAX,
		       LC898217XC_DAC_ABS_MAX);
}

static int lc898217xc_apply_focus(struct lc898217xc_dev *vcm, s32 focus)
{
	s16 dac = lc898217xc_focus_to_dac(focus);

	return lc898217xc_write_u16(vcm->client, LC898217XC_REG_DAC, (u16)dac);
}

static int lc898217xc_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct lc898217xc_dev *vcm = sd_to_lc898217xc(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(lc898217xc_supply_names),
				    vcm->supplies);
	if (ret)
		return ret;

	/* Wait for internal DSP/oscillator startup after VAF enable */
	for (int attempt = 0; attempt < 3; attempt++) {
		usleep_range(5000, 6000);
		ret = lc898217xc_write_u8(client, LC898217XC_REG_INIT,
					  LC898217XC_INIT_ACTIVE);
		if (!ret)
			break;
	}
	if (ret)
		goto err_disable;

	usleep_range(LC898217XC_INIT_DELAY_US, LC898217XC_INIT_DELAY_US + 1000);

	ret = lc898217xc_apply_focus(vcm, vcm->focus_pos);
	if (ret)
		goto err_disable;

	return 0;

err_disable:
	regulator_bulk_disable(ARRAY_SIZE(lc898217xc_supply_names),
			       vcm->supplies);
	return ret;
}

static int lc898217xc_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct lc898217xc_dev *vcm = sd_to_lc898217xc(sd);

	/* Return lens gently near rest before removing VAF */
	lc898217xc_apply_focus(vcm, LC898217XC_FOCUS_DEFAULT);
	usleep_range(1000, 2000);

	return regulator_bulk_disable(ARRAY_SIZE(lc898217xc_supply_names),
				      vcm->supplies);
}

static int lc898217xc_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct lc898217xc_dev *vcm = to_lc898217xc(ctrl);
	int ret = 0;

	if (ctrl->id != V4L2_CID_FOCUS_ABSOLUTE)
		return -EINVAL;

	mutex_lock(&vcm->lock);
	vcm->focus_pos = ctrl->val;
	if (pm_runtime_get_if_in_use(vcm->dev) > 0) {
		ret = lc898217xc_apply_focus(vcm, vcm->focus_pos);
		pm_runtime_put_autosuspend(vcm->dev);
	}
	mutex_unlock(&vcm->lock);

	return ret;
}

static const struct v4l2_ctrl_ops lc898217xc_ctrl_ops = {
	.s_ctrl = lc898217xc_set_ctrl,
};

static int lc898217xc_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct lc898217xc_dev *vcm = sd_to_lc898217xc(sd);

	return pm_runtime_resume_and_get(vcm->dev);
}

static int lc898217xc_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct lc898217xc_dev *vcm = sd_to_lc898217xc(sd);

	pm_runtime_put_autosuspend(vcm->dev);
	return 0;
}

static const struct v4l2_subdev_internal_ops lc898217xc_int_ops = {
	.open = lc898217xc_open,
	.close = lc898217xc_close,
};

static const struct v4l2_subdev_core_ops lc898217xc_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops lc898217xc_ops = {
	.core = &lc898217xc_core_ops,
};

static int lc898217xc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct lc898217xc_dev *vcm;
	u8 dac_raw[2] = {};
	u8 status_reg = 0;
	unsigned int i;
	int ret;

	vcm = devm_kzalloc(dev, sizeof(*vcm), GFP_KERNEL);
	if (!vcm)
		return -ENOMEM;

	vcm->dev = dev;
	vcm->client = client;
	vcm->focus_pos = LC898217XC_FOCUS_DEFAULT;
	mutex_init(&vcm->lock);

	for (i = 0; i < ARRAY_SIZE(lc898217xc_supply_names); i++)
		vcm->supplies[i].supply = lc898217xc_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(lc898217xc_supply_names),
				      vcm->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	v4l2_i2c_subdev_init(&vcm->sd, client, &lc898217xc_ops);
	vcm->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	vcm->sd.internal_ops = &lc898217xc_int_ops;

	v4l2_ctrl_handler_init(&vcm->ctrls, 1);
	v4l2_ctrl_new_std(&vcm->ctrls, &lc898217xc_ctrl_ops,
			  V4L2_CID_FOCUS_ABSOLUTE, 0, LC898217XC_FOCUS_MAX,
			  1, LC898217XC_FOCUS_DEFAULT);
	if (vcm->ctrls.error) {
		ret = vcm->ctrls.error;
		goto err_ctrl;
	}
	vcm->sd.ctrl_handler = &vcm->ctrls;

	ret = media_entity_pads_init(&vcm->sd.entity, 0, NULL);
	if (ret)
		goto err_ctrl;

	vcm->sd.entity.function = MEDIA_ENT_F_LENS;

	ret = lc898217xc_power_on(dev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to power on lc898217xc\n");
		goto err_media;
	}

	ret = lc898217xc_read_bytes(client, LC898217XC_REG_DAC, dac_raw,
				    sizeof(dac_raw));
	if (ret) {
		dev_err(dev, "DAC readback failed: %d\n", ret);
		goto err_power;
	}
	lc898217xc_read_bytes(client, LC898217XC_REG_STATUS, &status_reg, 1);
	dev_info(dev, "lc898217xc active at 0x%02x (dac=0x%04x, status=0x%02x)\n",
		 client->addr, get_unaligned_be16(dac_raw), status_reg);

	ret = v4l2_async_register_subdev(&vcm->sd);
	if (ret)
		goto err_power;

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_set_autosuspend_delay(dev, LC898217XC_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return 0;

err_power:
	lc898217xc_power_off(dev);
err_media:
	media_entity_cleanup(&vcm->sd.entity);
err_ctrl:
	v4l2_ctrl_handler_free(&vcm->ctrls);
	mutex_destroy(&vcm->lock);
	return ret;
}

static void lc898217xc_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct lc898217xc_dev *vcm = sd_to_lc898217xc(sd);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		lc898217xc_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&vcm->ctrls);
	mutex_destroy(&vcm->lock);
}

static const struct dev_pm_ops lc898217xc_pm_ops = {
	SET_RUNTIME_PM_OPS(lc898217xc_power_off, lc898217xc_power_on, NULL)
};

static const struct of_device_id lc898217xc_of_match[] = {
	{ .compatible = "onnn,lc898217xc" },
	{ }
};
MODULE_DEVICE_TABLE(of, lc898217xc_of_match);

static struct i2c_driver lc898217xc_i2c_driver = {
	.driver = {
		.name = "lc898217xc-op5t",
		.pm = pm_ptr(&lc898217xc_pm_ops),
		.of_match_table = lc898217xc_of_match,
	},
	.probe = lc898217xc_probe,
	.remove = lc898217xc_remove,
};
module_i2c_driver(lc898217xc_i2c_driver);

MODULE_DESCRIPTION("Clean-room ON Semi LC898217XC VCM driver for OnePlus 5T");
MODULE_LICENSE("GPL");
