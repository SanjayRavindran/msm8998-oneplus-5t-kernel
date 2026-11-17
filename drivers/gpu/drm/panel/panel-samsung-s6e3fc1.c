// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2021 Jami Kettunen <jami.kettunen@protonmail.com>
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/swab.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct s6e3fc1 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *supply;
	struct gpio_desc *poc_gpio;
	struct gpio_desc *vci_gpio;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	struct mutex lock;
	bool prepared;
	bool enabled;
	u8 color_mode; /* 0 = native, 1 = srgb, 2 = dci_p3 */
	bool hbm;
};

static void s6e3fc1_power_off(struct s6e3fc1 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	gpiod_set_value_cansleep(ctx->enable_gpio, 0);
	gpiod_set_value_cansleep(ctx->vci_gpio, 0);
	regulator_disable(ctx->supply);
	gpiod_set_value_cansleep(ctx->poc_gpio, 0);
}

static inline struct s6e3fc1 *to_s6e3fc1_panel(struct drm_panel *panel)
{
	return container_of(panel, struct s6e3fc1, panel);
}

#define dsi_dcs_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static void s6e3fc1_reset(struct s6e3fc1 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int s6e3fc1_on(struct s6e3fc1 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(25);

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		dev_err(dev, "Failed to set tear on: %d\n", ret);
		return ret;
	}

	dsi_dcs_write_seq(dsi, 0xfc, 0x5a, 0x5a);
	dsi_dcs_write_seq(dsi, 0xe8, 0x64, 0x08, 0x0c);
	dsi_dcs_write_seq(dsi, 0xfc, 0xa5, 0xa5);
	dsi_dcs_write_seq(dsi, 0xf0, 0x5a, 0x5a);
	dsi_dcs_write_seq(dsi, 0xb0, 0x01);
	dsi_dcs_write_seq(dsi, 0xed, 0x04);
	dsi_dcs_write_seq(dsi, 0xf0, 0xa5, 0xa5);
	dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_POWER_SAVE, 0x00);

	return 0;
}

static int s6e3fc1_off(struct s6e3fc1 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}
	usleep_range(10000, 11000);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(160);

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static int s6e3fc1_prepare(struct drm_panel *panel)
{
	struct s6e3fc1 *ctx = to_s6e3fc1_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret = 0;

	mutex_lock(&ctx->lock);
	if (ctx->prepared)
		goto out_unlock;

	gpiod_set_value_cansleep(ctx->poc_gpio, 1);
	usleep_range(10000, 11000);

	ret = regulator_enable(ctx->supply);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulator: %d\n", ret);
		gpiod_set_value_cansleep(ctx->poc_gpio, 0);
		goto out_unlock;
	}
	usleep_range(10000, 11000);

	gpiod_set_value_cansleep(ctx->vci_gpio, 1);
	usleep_range(10000, 11000);

	gpiod_set_value_cansleep(ctx->enable_gpio, 1);
	usleep_range(10000, 11000);

	s6e3fc1_reset(ctx);

	ret = s6e3fc1_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		s6e3fc1_power_off(ctx);
		goto out_unlock;
	}

	dev_info(dev, "s6e3fc1 prepared cleanly\n");
	ctx->prepared = true;

out_unlock:
	mutex_unlock(&ctx->lock);
	return ret;
}

static int s6e3fc1_unprepare(struct drm_panel *panel)
{
	struct s6e3fc1 *ctx = to_s6e3fc1_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	mutex_lock(&ctx->lock);
	if (!ctx->prepared)
		goto out_unlock;

	ret = s6e3fc1_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	s6e3fc1_power_off(ctx);

	ctx->prepared = false;
	ctx->enabled = false;

out_unlock:
	mutex_unlock(&ctx->lock);
	return 0;
}

static int s6e3fc1_apply_color_mode_locked(struct s6e3fc1 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;

	switch (ctx->color_mode) {
	case 1: /* sRGB */
		dsi_dcs_write_seq(dsi, 0xf0, 0x5a, 0x5a);
		dsi_dcs_write_seq(dsi, 0xbc, 0x01);
		dsi_dcs_write_seq(dsi, 0xb0, 0x01);
		dsi_dcs_write_seq(dsi, 0xbc, 0x12);
		dsi_dcs_write_seq(dsi, 0xb0, 0x2c);
		dsi_dcs_write_seq(dsi, 0xbc, 0xa3, 0x05, 0x04, 0x46, 0xcd, 0x10, 0x05,
				  0x09, 0xb0, 0x57, 0xef, 0xcf, 0xbb, 0x11, 0xbf, 0xe1,
				  0xda, 0x17, 0xff, 0xf9, 0xd8);
		dsi_dcs_write_seq(dsi, 0xb0, 0x42);
		dsi_dcs_write_seq(dsi, 0xbc, 0x03);
		dsi_dcs_write_seq(dsi, 0xb0, 0x4b);
		dsi_dcs_write_seq(dsi, 0xbc, 0xa1);
		dsi_dcs_write_seq(dsi, 0xf0, 0xa5, 0xa5);
		break;
	case 2: /* DCI-P3 */
		dsi_dcs_write_seq(dsi, 0xf0, 0x5a, 0x5a);
		dsi_dcs_write_seq(dsi, 0xbc, 0x01);
		dsi_dcs_write_seq(dsi, 0xb0, 0x01);
		dsi_dcs_write_seq(dsi, 0xbc, 0x12);
		dsi_dcs_write_seq(dsi, 0xb0, 0x2c);
		dsi_dcs_write_seq(dsi, 0xbc, 0xc6, 0x00, 0x00, 0x1e, 0xcf, 0x00, 0x06,
				  0x0a, 0xc3, 0x26, 0xef, 0xcd, 0xe0, 0x04, 0xce, 0xe9,
				  0xdf, 0x00, 0xff, 0xf9, 0xd8);
		dsi_dcs_write_seq(dsi, 0xb0, 0x42);
		dsi_dcs_write_seq(dsi, 0xbc, 0x03);
		dsi_dcs_write_seq(dsi, 0xb0, 0x4b);
		dsi_dcs_write_seq(dsi, 0xbc, 0xa1);
		dsi_dcs_write_seq(dsi, 0xf0, 0xa5, 0xa5);
		break;
	default: /* Native */
		dsi_dcs_write_seq(dsi, 0xf0, 0x5a, 0x5a);
		dsi_dcs_write_seq(dsi, 0xbc, 0xe0, 0x00);
		dsi_dcs_write_seq(dsi, 0xf0, 0xa5, 0xa5);
		break;
	}
	return 0;
}

static int s6e3fc1_apply_hbm_locked(struct s6e3fc1 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;

	if (ctx->hbm)
		dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0xe8);
	else
		dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	return 0;
}

static int s6e3fc1_enable(struct drm_panel *panel)
{
	struct s6e3fc1 *ctx = to_s6e3fc1_panel(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	u8 rddpm = 0, rddsm = 0;
	int ret = 0, r1, r2;

	mutex_lock(&ctx->lock);
	if (ctx->enabled)
		goto out_unlock;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display on in enable: %d\n", ret);
		goto out_unlock;
	}

	usleep_range(5000, 6000);
	r1 = mipi_dsi_dcs_read(dsi, MIPI_DCS_GET_POWER_MODE, &rddpm, 1);
	r2 = mipi_dsi_dcs_read(dsi, MIPI_DCS_GET_SIGNAL_MODE, &rddsm, 1);
	dev_info(dev, "s6e3fc1_enable: RDDPM=0x%02x (%d) RDDSM=0x%02x (%d)\n",
		 rddpm, r1, rddsm, r2);

	if (ctx->color_mode)
		s6e3fc1_apply_color_mode_locked(ctx);
	if (ctx->hbm)
		s6e3fc1_apply_hbm_locked(ctx);

	ctx->enabled = true;

out_unlock:
	mutex_unlock(&ctx->lock);
	return ret;
}

static int s6e3fc1_disable(struct drm_panel *panel)
{
	struct s6e3fc1 *ctx = to_s6e3fc1_panel(panel);

	mutex_lock(&ctx->lock);
	ctx->enabled = false;
	mutex_unlock(&ctx->lock);
	return 0;
}

static const struct drm_display_mode s6e3fc1_mode = {
	.clock = (1080 + 128 + 16 + 64) * (2160 + 18 + 2 + 4) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 128,
	.hsync_end = 1080 + 128 + 16,
	.htotal = 1080 + 128 + 16 + 64,
	.vdisplay = 2160,
	.vsync_start = 2160 + 18,
	.vsync_end = 2160 + 18 + 2,
	.vtotal = 2160 + 18 + 2 + 4,
	.width_mm = 68,
	.height_mm = 137,
};

static int s6e3fc1_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &s6e3fc1_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs s6e3fc1_panel_funcs = {
	.prepare = s6e3fc1_prepare,
	.unprepare = s6e3fc1_unprepare,
	.enable = s6e3fc1_enable,
	.disable = s6e3fc1_disable,
	.get_modes = s6e3fc1_get_modes,
};


static ssize_t srgb_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s6e3fc1 *ctx = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%u\n", ctx->color_mode == 1 ? 1 : 0);
}

static ssize_t srgb_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct s6e3fc1 *ctx = dev_get_drvdata(dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	mutex_lock(&ctx->lock);
	ctx->color_mode = val ? 1 : 0;
	if (ctx->prepared && ctx->enabled)
		ret = s6e3fc1_apply_color_mode_locked(ctx);
	mutex_unlock(&ctx->lock);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(srgb);

static ssize_t dci_p3_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s6e3fc1 *ctx = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%u\n", ctx->color_mode == 2 ? 1 : 0);
}

static ssize_t dci_p3_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct s6e3fc1 *ctx = dev_get_drvdata(dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	mutex_lock(&ctx->lock);
	ctx->color_mode = val ? 2 : 0;
	if (ctx->prepared && ctx->enabled)
		ret = s6e3fc1_apply_color_mode_locked(ctx);
	mutex_unlock(&ctx->lock);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(dci_p3);

static ssize_t hbm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s6e3fc1 *ctx = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%u\n", ctx->hbm ? 1 : 0);
}

static ssize_t hbm_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct s6e3fc1 *ctx = dev_get_drvdata(dev);
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	mutex_lock(&ctx->lock);
	ctx->hbm = val;
	if (ctx->prepared && ctx->enabled)
		ret = s6e3fc1_apply_hbm_locked(ctx);
	mutex_unlock(&ctx->lock);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(hbm);

static struct attribute *s6e3fc1_attrs[] = {
	&dev_attr_srgb.attr,
	&dev_attr_dci_p3.attr,
	&dev_attr_hbm.attr,
	NULL
};
ATTRIBUTE_GROUPS(s6e3fc1);

static int s6e3fc1_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct s6e3fc1 *ctx = mipi_dsi_get_drvdata(dsi);
	u16 brightness;
	int ret;

	mutex_lock(&ctx->lock);
	if (!ctx->prepared || !ctx->enabled) {
		mutex_unlock(&ctx->lock);
		return 0;
	}

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	brightness = (u16)backlight_get_brightness(bl);
	/* Panel expects big-endian 10-bit DBV in MIPI DCS 0x51 */
	brightness = __swab16(brightness);

	ret = mipi_dsi_dcs_set_display_brightness(dsi, brightness);
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	mutex_unlock(&ctx->lock);
	if (ret < 0)
		return ret;

	return 0;
}

static int s6e3fc1_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct s6e3fc1 *ctx = mipi_dsi_get_drvdata(dsi);
	u16 brightness;
	int ret;

	mutex_lock(&ctx->lock);
	if (!ctx->prepared || !ctx->enabled) {
		ret = bl->props.brightness;
		mutex_unlock(&ctx->lock);
		return ret;
	}

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness(dsi, &brightness);
	mutex_unlock(&ctx->lock);
	if (ret < 0)
		return ret;

	return __swab16(brightness);
}

static const struct backlight_ops s6e3fc1_bl_ops = {
	.update_status = s6e3fc1_bl_update_status,
	.get_brightness = s6e3fc1_bl_get_brightness,
};

static struct backlight_device *
s6e3fc1_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 51,
		.max_brightness = 1023,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &s6e3fc1_bl_ops, &props);
}

static int s6e3fc1_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct s6e3fc1 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct s6e3fc1, panel,
				   &s6e3fc1_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(ctx->supply))
		return dev_err_probe(dev, PTR_ERR(ctx->supply),
				     "Failed to get vddio regulator\n");

	ctx->poc_gpio = devm_gpiod_get_optional(dev, "poc", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->poc_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->poc_gpio),
				     "Failed to get poc-gpios\n");

	ctx->vci_gpio = devm_gpiod_get_optional(dev, "vci", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->vci_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->vci_gpio),
				     "Failed to get vci-gpios\n");

	ctx->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->enable_gpio),
				     "Failed to get enable-gpios\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	mutex_init(&ctx->lock);
	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = s6e3fc1_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	if (sysfs_create_groups(&dev->kobj, s6e3fc1_groups))
		dev_warn(dev, "Failed to create s6e3fc1 sysfs groups\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		sysfs_remove_groups(&dsi->dev.kobj, s6e3fc1_groups);
	drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void s6e3fc1_remove(struct mipi_dsi_device *dsi)
{
	struct s6e3fc1 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id s6e3fc1_of_match[] = {
	{ .compatible = "samsung,s6e3fc1" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s6e3fc1_of_match);

static struct mipi_dsi_driver s6e3fc1_driver = {
	.probe = s6e3fc1_probe,
	.remove = s6e3fc1_remove,
	.driver = {
		.name = "panel-samsung-s6e3fc1",
		.of_match_table = s6e3fc1_of_match,
	},
};
module_mipi_dsi_driver(s6e3fc1_driver);

MODULE_AUTHOR("Jami Kettunen <jami.kettunen@protonmail.com>");
MODULE_DESCRIPTION("DRM driver for Samsung S6E3FC1 AMOLED DSI cmd mode panel found on OnePlus 5T phones");
MODULE_LICENSE("GPL");
