// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clean-room V4L2 I2C sensor driver for Sony IMX398, IMX376K, and IMX371
 * on Qualcomm MSM8998 (OnePlus 5T / dumpling).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

#include "sony-imx-op5t-tables.h"

#define SONY_REG_CHIP_ID		CCI_REG16(0x0016)
#define SONY_REG_MODE_SELECT		CCI_REG8(0x0100)
#define SONY_MODE_STANDBY		0x00
#define SONY_MODE_STREAMING		0x01
#define SONY_REG_ORIENTATION		CCI_REG8(0x0101)
#define SONY_REG_SW_RESET		CCI_REG8(0x0103)
#define SONY_REG_EXPOSURE		CCI_REG16(0x0202)
#define SONY_REG_ANA_GAIN		CCI_REG16(0x0204)
#define SONY_REG_FLL			CCI_REG16(0x0340)
#define SONY_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define SONY_MCLK_FREQ_HZ		24000000
#define SONY_VTS_MAX			0xffff
#define SONY_EXPOSURE_MIN		1
#define SONY_EXPOSURE_MARGIN		8
#define SONY_MAX_SUPPLIES		4

struct sony_op5t_chip_info {
	const char *name;
	u16 chip_id;
	u16 id_mask;
	u32 native_width;
	u32 native_height;
	s32 ana_gain_max;
	const char * const *supply_names;
	unsigned int num_supplies;
	struct sony_op5t_reg_list init_regs;
	const struct sony_op5t_mode *modes;
	unsigned int num_modes;
	const s64 *link_freqs;
	unsigned int num_link_freqs;
};

struct sony_op5t {
	struct device *dev;
	const struct sony_op5t_chip_info *chip;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *mclk;
	struct regulator_bulk_data supplies[SONY_MAX_SUPPLIES];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *vdig_gpio;
	struct gpio_desc *vana_gpio;
	struct gpio_desc *vaf_gpio;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;

	const struct sony_op5t_mode *cur_mode;
	struct mutex mutex;
	bool streaming;
};

static const u32 sony_bayer_codes[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,
};

static const char * const sony_test_pattern_menu[] = {
	"Disabled",
	"Solid Color",
	"100% Color Bars",
	"Fade to Grey Color Bars",
	"PN9",
};

static const char * const sony_4_supplies[] = {
	"vio",
	"vana",
	"vdig",
	"vaf",
};

static const char * const sony_3_supplies[] = {
	"vio",
	"vana",
	"vdig",
};

static const struct sony_op5t_chip_info imx398_info = {
	.name = "imx398",
	.chip_id = 0x0098,
	.id_mask = 0xf0ff,
	.native_width = 4656,
	.native_height = 3496,
	.ana_gain_max = 448,
	.supply_names = sony_4_supplies,
	.num_supplies = ARRAY_SIZE(sony_4_supplies),
	.init_regs = {
		.regs = imx398_init_regs,
		.num_of_regs = ARRAY_SIZE(imx398_init_regs),
	},
	.modes = imx398_modes,
	.num_modes = ARRAY_SIZE(imx398_modes),
	.link_freqs = imx398_link_freqs,
	.num_link_freqs = ARRAY_SIZE(imx398_link_freqs),
};

static const struct sony_op5t_chip_info imx376k_info = {
	.name = "imx376k",
	.chip_id = 0x0376,
	.id_mask = 0xffff,
	.native_width = 2592,
	.native_height = 1940,
	.ana_gain_max = 960,
	.supply_names = sony_4_supplies,
	.num_supplies = ARRAY_SIZE(sony_4_supplies),
	.init_regs = {
		.regs = imx376k_init_regs,
		.num_of_regs = ARRAY_SIZE(imx376k_init_regs),
	},
	.modes = imx376k_modes,
	.num_modes = ARRAY_SIZE(imx376k_modes),
	.link_freqs = imx376k_link_freqs,
	.num_link_freqs = ARRAY_SIZE(imx376k_link_freqs),
};

static const struct sony_op5t_chip_info imx371_info = {
	.name = "imx371",
	.chip_id = 0x0301,
	.id_mask = 0xff0f,
	.native_width = 4656,
	.native_height = 3456,
	.ana_gain_max = 960,
	.supply_names = sony_3_supplies,
	.num_supplies = ARRAY_SIZE(sony_3_supplies),
	.init_regs = {
		.regs = imx371_init_regs,
		.num_of_regs = ARRAY_SIZE(imx371_init_regs),
	},
	.modes = imx371_modes,
	.num_modes = ARRAY_SIZE(imx371_modes),
	.link_freqs = imx371_link_freqs,
	.num_link_freqs = ARRAY_SIZE(imx371_link_freqs),
};

static inline struct sony_op5t *to_sony_op5t(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sony_op5t, sd);
}

static u32 sony_op5t_get_format_code(const struct sony_op5t *sensor)
{
	unsigned int idx;

	lockdep_assert_held(&sensor->mutex);

	idx = (sensor->vflip->val ? 2 : 0) | (sensor->hflip->val ? 1 : 0);
	return sony_bayer_codes[idx];
}

static void sony_op5t_update_pad_format(const struct sony_op5t *sensor,
					const struct sony_op5t_mode *mode,
					struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = sony_op5t_get_format_code(sensor);
	fmt->format.field = V4L2_FIELD_NONE;
}

static int sony_op5t_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct sony_op5t *sensor = to_sony_op5t(sd);
	const struct sony_op5t_mode *def_mode = &sensor->chip->modes[0];
	struct v4l2_mbus_framefmt *try_fmt =
		v4l2_subdev_state_get_format(fh->state, 0);
	struct v4l2_rect *try_crop =
		v4l2_subdev_state_get_crop(fh->state, 0);

	mutex_lock(&sensor->mutex);
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = sony_op5t_get_format_code(sensor);
	try_fmt->field = V4L2_FIELD_NONE;
	mutex_unlock(&sensor->mutex);

	try_crop->left = 0;
	try_crop->top = 0;
	try_crop->width = sensor->chip->native_width;
	try_crop->height = sensor->chip->native_height;

	return 0;
}

static void sony_op5t_adjust_exposure_range(struct sony_op5t *sensor)
{
	int exp_max, exp_def;

	exp_max = sensor->cur_mode->height + sensor->vblank->val -
		  SONY_EXPOSURE_MARGIN;
	if (exp_max < SONY_EXPOSURE_MIN)
		exp_max = SONY_EXPOSURE_MIN;
	exp_def = min(exp_max, sensor->exposure->val);
	__v4l2_ctrl_modify_range(sensor->exposure, SONY_EXPOSURE_MIN,
				 exp_max, 1, exp_def);
}

static int sony_op5t_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sony_op5t *sensor =
		container_of(ctrl->handler, struct sony_op5t, ctrl_handler);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK)
		sony_op5t_adjust_exposure_range(sensor);

	if (pm_runtime_get_if_in_use(sensor->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(sensor->regmap, SONY_REG_ANA_GAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(sensor->regmap, SONY_REG_EXPOSURE,
				ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(sensor->regmap, SONY_REG_FLL,
				sensor->cur_mode->height + ctrl->val, NULL);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = cci_write(sensor->regmap, SONY_REG_ORIENTATION,
				(sensor->hflip->val ? BIT(0) : 0) |
				(sensor->vflip->val ? BIT(1) : 0),
				NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(sensor->regmap, SONY_REG_TEST_PATTERN,
				ctrl->val, NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(sensor->dev);
	return ret;
}

static const struct v4l2_ctrl_ops sony_op5t_ctrl_ops = {
	.s_ctrl = sony_op5t_set_ctrl,
};

static int sony_op5t_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	struct sony_op5t *sensor = to_sony_op5t(sd);

	if (code->index > 0)
		return -EINVAL;

	mutex_lock(&sensor->mutex);
	code->code = sony_op5t_get_format_code(sensor);
	mutex_unlock(&sensor->mutex);

	return 0;
}

static int sony_op5t_enum_frame_size(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *sd_state,
				     struct v4l2_subdev_frame_size_enum *fse)
{
	struct sony_op5t *sensor = to_sony_op5t(sd);
	u32 code;

	if (fse->index >= sensor->chip->num_modes)
		return -EINVAL;

	mutex_lock(&sensor->mutex);
	code = sony_op5t_get_format_code(sensor);
	mutex_unlock(&sensor->mutex);

	if (fse->code != code)
		return -EINVAL;

	fse->min_width = sensor->chip->modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = sensor->chip->modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int sony_op5t_get_pad_format(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_format *fmt)
{
	struct sony_op5t *sensor = to_sony_op5t(sd);

	mutex_lock(&sensor->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt->format = *v4l2_subdev_state_get_format(sd_state, fmt->pad);
	else
		sony_op5t_update_pad_format(sensor, sensor->cur_mode, fmt);
	mutex_unlock(&sensor->mutex);

	return 0;
}

static int sony_op5t_set_pad_format(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_format *fmt)
{
	struct sony_op5t *sensor = to_sony_op5t(sd);
	const struct sony_op5t_mode *mode;
	s32 vblank_def, vblank_min, hblank;

	mutex_lock(&sensor->mutex);

	if (sensor->streaming && fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		mutex_unlock(&sensor->mutex);
		return -EBUSY;
	}

	mode = v4l2_find_nearest_size(sensor->chip->modes,
				      sensor->chip->num_modes,
				      width, height,
				      fmt->format.width, fmt->format.height);
	sony_op5t_update_pad_format(sensor, mode, fmt);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(sd_state, fmt->pad) = fmt->format;
	} else {
		sensor->cur_mode = mode;
		__v4l2_ctrl_s_ctrl(sensor->link_freq, mode->link_freq_index);
		__v4l2_ctrl_modify_range(sensor->pixel_rate,
					 mode->pixel_rate, mode->pixel_rate,
					 1, mode->pixel_rate);
		vblank_def = mode->vts_def - mode->height;
		vblank_min = mode->vts_min - mode->height;
		__v4l2_ctrl_modify_range(sensor->vblank, vblank_min,
					 SONY_VTS_MAX - mode->height,
					 1, vblank_def);
		__v4l2_ctrl_s_ctrl(sensor->vblank, vblank_def);
		hblank = mode->llp - mode->width;
		__v4l2_ctrl_modify_range(sensor->hblank, hblank, hblank,
					 1, hblank);
	}

	mutex_unlock(&sensor->mutex);
	return 0;
}

static int sony_op5t_get_selection(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_selection *sel)
{
	struct sony_op5t *sensor = to_sony_op5t(sd);

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		mutex_lock(&sensor->mutex);
		if (sel->which == V4L2_SUBDEV_FORMAT_TRY)
			sel->r = *v4l2_subdev_state_get_crop(sd_state, sel->pad);
		else
			sel->r = sensor->cur_mode->crop;
		mutex_unlock(&sensor->mutex);
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = sensor->chip->native_width;
		sel->r.height = sensor->chip->native_height;
		return 0;
	default:
		return -EINVAL;
	}
}

static int sony_op5t_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sony_op5t *sensor = to_sony_op5t(sd);
	int ret;

	gpiod_set_value_cansleep(sensor->reset_gpio, 1);

	ret = regulator_bulk_enable(sensor->chip->num_supplies,
				    sensor->supplies);
	if (ret) {
		dev_err(dev, "failed to enable supplies: %d\n", ret);
		return ret;
	}

	gpiod_set_value_cansleep(sensor->vdig_gpio, 1);
	gpiod_set_value_cansleep(sensor->vana_gpio, 1);
	gpiod_set_value_cansleep(sensor->vaf_gpio, 1);
	usleep_range(2000, 3000);

	ret = clk_set_rate(sensor->mclk, SONY_MCLK_FREQ_HZ);
	if (ret)
		dev_warn(dev, "failed to set mclk rate: %d\n", ret);

	ret = clk_prepare_enable(sensor->mclk);
	if (ret) {
		dev_err(dev, "failed to enable mclk: %d\n", ret);
		goto err_gpios_off;
	}

	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(10000, 12000);

	return 0;

err_gpios_off:
	gpiod_set_value_cansleep(sensor->vaf_gpio, 0);
	gpiod_set_value_cansleep(sensor->vana_gpio, 0);
	gpiod_set_value_cansleep(sensor->vdig_gpio, 0);
	regulator_bulk_disable(sensor->chip->num_supplies, sensor->supplies);
	return ret;
}

static int sony_op5t_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sony_op5t *sensor = to_sony_op5t(sd);

	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	clk_disable_unprepare(sensor->mclk);
	gpiod_set_value_cansleep(sensor->vaf_gpio, 0);
	gpiod_set_value_cansleep(sensor->vana_gpio, 0);
	gpiod_set_value_cansleep(sensor->vdig_gpio, 0);
	regulator_bulk_disable(sensor->chip->num_supplies, sensor->supplies);

	return 0;
}

static int sony_op5t_start_streaming(struct sony_op5t *sensor)
{
	const struct sony_op5t_chip_info *chip = sensor->chip;
	const struct sony_op5t_mode *mode = sensor->cur_mode;
	int ret;

	ret = cci_write(sensor->regmap, SONY_REG_SW_RESET, 0x01, NULL);
	if (ret) {
		dev_err(sensor->dev, "sw reset failed: %d\n", ret);
		return ret;
	}
	fsleep(12000);

	ret = cci_multi_reg_write(sensor->regmap, chip->init_regs.regs,
				  chip->init_regs.num_of_regs, NULL);
	if (ret) {
		dev_err(sensor->dev, "init sequence failed: %d\n", ret);
		return ret;
	}

	ret = cci_multi_reg_write(sensor->regmap, mode->reg_list.regs,
				  mode->reg_list.num_of_regs, NULL);
	if (ret) {
		dev_err(sensor->dev, "mode sequence failed: %d\n", ret);
		return ret;
	}

	ret = __v4l2_ctrl_handler_setup(&sensor->ctrl_handler);
	if (ret)
		return ret;

	return cci_write(sensor->regmap, SONY_REG_MODE_SELECT,
			 SONY_MODE_STREAMING, NULL);
}

static int sony_op5t_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct sony_op5t *sensor = to_sony_op5t(sd);
	int ret = 0;

	mutex_lock(&sensor->mutex);

	if (sensor->streaming == !!enable) {
		mutex_unlock(&sensor->mutex);
		return 0;
	}

	if (enable) {
		ret = pm_runtime_resume_and_get(sensor->dev);
		if (ret < 0)
			goto err_unlock;

		ret = sony_op5t_start_streaming(sensor);
		if (ret) {
			pm_runtime_put(sensor->dev);
			goto err_unlock;
		}
		sensor->streaming = true;
	} else {
		cci_write(sensor->regmap, SONY_REG_MODE_SELECT,
			  SONY_MODE_STANDBY, NULL);
		pm_runtime_put(sensor->dev);
		sensor->streaming = false;
	}

err_unlock:
	mutex_unlock(&sensor->mutex);
	return ret;
}

static int sony_op5t_identify_module(struct sony_op5t *sensor)
{
	u64 val = 0;
	int ret;

	ret = cci_read(sensor->regmap, SONY_REG_CHIP_ID, &val, NULL);
	if (ret) {
		dev_err(sensor->dev, "%s: failed to read chip id: %d\n",
			sensor->chip->name, ret);
		return ret;
	}

	if ((val & sensor->chip->id_mask) != sensor->chip->chip_id) {
		dev_err(sensor->dev, "%s: chip id mismatch: 0x%04llx != 0x%04x\n",
			sensor->chip->name, val, sensor->chip->chip_id);
		return -EIO;
	}

	dev_info(sensor->dev, "identified %s (chip_id=0x%04llx)\n",
		 sensor->chip->name, val);
	return 0;
}

static const struct v4l2_subdev_video_ops sony_op5t_video_ops = {
	.s_stream = sony_op5t_set_stream,
};

static const struct v4l2_subdev_pad_ops sony_op5t_pad_ops = {
	.enum_mbus_code = sony_op5t_enum_mbus_code,
	.enum_frame_size = sony_op5t_enum_frame_size,
	.get_fmt = sony_op5t_get_pad_format,
	.set_fmt = sony_op5t_set_pad_format,
	.get_selection = sony_op5t_get_selection,
};

static const struct v4l2_subdev_ops sony_op5t_subdev_ops = {
	.video = &sony_op5t_video_ops,
	.pad = &sony_op5t_pad_ops,
};

static const struct v4l2_subdev_internal_ops sony_op5t_internal_ops = {
	.open = sony_op5t_open,
};

static int sony_op5t_init_controls(struct sony_op5t *sensor)
{
	const struct sony_op5t_chip_info *chip = sensor->chip;
	const struct sony_op5t_mode *mode = sensor->cur_mode;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *hdlr = &sensor->ctrl_handler;
	s64 vblank_def = mode->vts_def - mode->height;
	s64 vblank_min = mode->vts_min - mode->height;
	s64 hblank = mode->llp - mode->width;
	s64 exp_max = mode->vts_def - SONY_EXPOSURE_MARGIN;
	int ret;

	ret = v4l2_ctrl_handler_init(hdlr, 10);
	if (ret)
		return ret;

	hdlr->lock = &sensor->mutex;

	sensor->link_freq = v4l2_ctrl_new_int_menu(hdlr, &sony_op5t_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   chip->num_link_freqs - 1,
						   mode->link_freq_index,
						   chip->link_freqs);
	if (sensor->link_freq)
		sensor->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->pixel_rate = v4l2_ctrl_new_std(hdlr, &sony_op5t_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       mode->pixel_rate,
					       mode->pixel_rate, 1,
					       mode->pixel_rate);
	if (sensor->pixel_rate)
		sensor->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->hflip = v4l2_ctrl_new_std(hdlr, &sony_op5t_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (sensor->hflip)
		sensor->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	sensor->vflip = v4l2_ctrl_new_std(hdlr, &sony_op5t_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (sensor->vflip)
		sensor->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	sensor->vblank = v4l2_ctrl_new_std(hdlr, &sony_op5t_ctrl_ops,
					   V4L2_CID_VBLANK, vblank_min,
					   SONY_VTS_MAX - mode->height, 1,
					   vblank_def);

	sensor->hblank = v4l2_ctrl_new_std(hdlr, &sony_op5t_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank,
					   1, hblank);
	if (sensor->hblank)
		sensor->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->exposure = v4l2_ctrl_new_std(hdlr, &sony_op5t_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     SONY_EXPOSURE_MIN, exp_max,
					     1, exp_max);

	v4l2_ctrl_new_std(hdlr, &sony_op5t_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  0, chip->ana_gain_max, 1, 0);

	v4l2_ctrl_new_std_menu_items(hdlr, &sony_op5t_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(sony_test_pattern_menu) - 1,
				     0, 0, sony_test_pattern_menu);

	if (hdlr->error) {
		ret = hdlr->error;
		goto err_free;
	}

	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret)
		goto err_free;

	ret = v4l2_ctrl_new_fwnode_properties(hdlr, &sony_op5t_ctrl_ops, &props);
	if (ret)
		goto err_free;

	sensor->sd.ctrl_handler = hdlr;
	return 0;

err_free:
	v4l2_ctrl_handler_free(hdlr);
	return ret;
}

static int sony_op5t_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sony_op5t *sensor;
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned int i;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = dev;
	sensor->chip = device_get_match_data(dev);
	if (!sensor->chip)
		return -EINVAL;

	mutex_init(&sensor->mutex);

	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap),
				     "failed to init CCI regmap\n");

	for (i = 0; i < sensor->chip->num_supplies; i++)
		sensor->supplies[i].supply = sensor->chip->supply_names[i];

	ret = devm_regulator_bulk_get(dev, sensor->chip->num_supplies,
				      sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get supplies\n");

	sensor->mclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sensor->mclk))
		return dev_err_probe(dev, PTR_ERR(sensor->mclk),
				     "failed to get mclk\n");

	sensor->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset gpio\n");

	sensor->vdig_gpio = devm_gpiod_get_optional(dev, "vdig", GPIOD_OUT_LOW);
	if (IS_ERR(sensor->vdig_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->vdig_gpio),
				     "failed to get vdig gpio\n");

	sensor->vana_gpio = devm_gpiod_get_optional(dev, "vana", GPIOD_OUT_LOW);
	if (IS_ERR(sensor->vana_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->vana_gpio),
				     "failed to get vana gpio\n");

	sensor->vaf_gpio = devm_gpiod_get_optional(dev, "vaf", GPIOD_OUT_LOW);
	if (IS_ERR(sensor->vaf_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->vaf_gpio),
				     "failed to get vaf gpio\n");

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint)
		return -EINVAL;

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret)
		return ret;

	if (ep.bus.mipi_csi2.num_data_lanes != 4) {
		dev_err(dev, "expected 4 CSI-2 lanes, got %u\n",
			ep.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto err_ep_free;
	}

	{
		unsigned long lf_bitmap = 0;

		ret = v4l2_link_freq_to_bitmap(dev, ep.link_frequencies,
					       ep.nr_of_link_frequencies,
					       sensor->chip->link_freqs,
					       sensor->chip->num_link_freqs,
					       &lf_bitmap);
		if (ret) {
			dev_err(dev, "DT link-frequencies mismatch: %d\n", ret);
			goto err_ep_free;
		}
	}

	sensor->cur_mode = &sensor->chip->modes[0];

	v4l2_i2c_subdev_init(&sensor->sd, client, &sony_op5t_subdev_ops);

	ret = sony_op5t_power_on(dev);
	if (ret)
		goto err_ep_free;

	ret = sony_op5t_identify_module(sensor);
	if (ret)
		goto err_power_off;

	ret = sony_op5t_init_controls(sensor);
	if (ret)
		goto err_power_off;

	sensor->sd.internal_ops = &sony_op5t_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto err_ctrl_free;

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret < 0)
		goto err_media;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);
	v4l2_fwnode_endpoint_free(&ep);

	return 0;

err_media:
	media_entity_cleanup(&sensor->sd.entity);
err_ctrl_free:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
err_power_off:
	sony_op5t_power_off(dev);
err_ep_free:
	v4l2_fwnode_endpoint_free(&ep);
	mutex_destroy(&sensor->mutex);
	return ret;
}

static void sony_op5t_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sony_op5t *sensor = to_sony_op5t(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	pm_runtime_disable(sensor->dev);
	if (!pm_runtime_status_suspended(sensor->dev))
		sony_op5t_power_off(sensor->dev);
	pm_runtime_set_suspended(sensor->dev);
	mutex_destroy(&sensor->mutex);
}

static const struct dev_pm_ops sony_op5t_pm_ops = {
	SET_RUNTIME_PM_OPS(sony_op5t_power_off, sony_op5t_power_on, NULL)
};

static const struct of_device_id sony_op5t_dt_ids[] = {
	{ .compatible = "sony,imx398", .data = &imx398_info },
	{ .compatible = "sony,imx376k", .data = &imx376k_info },
	{ .compatible = "sony,imx371", .data = &imx371_info },
	{ }
};
MODULE_DEVICE_TABLE(of, sony_op5t_dt_ids);

static struct i2c_driver sony_op5t_i2c_driver = {
	.driver = {
		.name = "sony-imx-op5t",
		.pm = pm_ptr(&sony_op5t_pm_ops),
		.of_match_table = sony_op5t_dt_ids,
	},
	.probe = sony_op5t_probe,
	.remove = sony_op5t_remove,
};
module_i2c_driver(sony_op5t_i2c_driver);

MODULE_DESCRIPTION("Sony IMX398/IMX376K/IMX371 V4L2 sensor driver");
MODULE_LICENSE("GPL");
