/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_GOODIX_FP_H
#define _UAPI_LINUX_GOODIX_FP_H

#include <linux/ioctl.h>
#include <linux/types.h>

enum gf_fp_mode {
	GF_IMAGE_MODE = 0,
	GF_KEY_MODE,
	GF_SLEEP_MODE,
	GF_FF_MODE,
	GF_DEBUG_MODE = 0x56,
};

typedef enum gf_nav_event {
	GF_NAV_NONE = 0,
	GF_NAV_FINGER_UP,
	GF_NAV_FINGER_DOWN,
	GF_NAV_UP,
	GF_NAV_DOWN,
	GF_NAV_LEFT,
	GF_NAV_RIGHT,
	GF_NAV_CLICK,
	GF_NAV_HEAVY,
	GF_NAV_LONG_PRESS,
	GF_NAV_DOUBLE_CLICK,
	GF_NAV_F2,
} gf_nav_event_t;

typedef enum gf_key_event {
	GF_KEY_NONE = 0,
	GF_KEY_HOME,
	GF_KEY_POWER,
	GF_KEY_MENU,
	GF_KEY_BACK,
	GF_KEY_CAMERA,
} gf_key_event_t;

struct gf_key {
	enum gf_key_event key;
	__u32 value;
};

struct gf_ioc_chip_info {
	__u8 vendor_id;
	__u8 mode;
	__u8 operation;
	__u8 reserved[5];
};

#define GF_IOC_MAGIC		'g'
#define GF_IOC_INIT		_IOR(GF_IOC_MAGIC, 0, __u8)
#define GF_IOC_EXIT		_IO(GF_IOC_MAGIC, 1)
#define GF_IOC_RESET		_IO(GF_IOC_MAGIC, 2)
#define GF_IOC_ENABLE_IRQ	_IO(GF_IOC_MAGIC, 3)
#define GF_IOC_DISABLE_IRQ	_IO(GF_IOC_MAGIC, 4)
#define GF_IOC_ENABLE_SPI_CLK	_IOW(GF_IOC_MAGIC, 5, __u32)
#define GF_IOC_DISABLE_SPI_CLK	_IO(GF_IOC_MAGIC, 6)
#define GF_IOC_ENABLE_POWER	_IO(GF_IOC_MAGIC, 7)
#define GF_IOC_DISABLE_POWER	_IO(GF_IOC_MAGIC, 8)
#define GF_IOC_INPUT_KEY_EVENT	_IOW(GF_IOC_MAGIC, 9, struct gf_key)
#define GF_IOC_ENTER_SLEEP_MODE	_IO(GF_IOC_MAGIC, 10)
#define GF_IOC_GET_FW_INFO	_IOR(GF_IOC_MAGIC, 11, __u8)
#define GF_IOC_REMOVE		_IO(GF_IOC_MAGIC, 12)
#define GF_IOC_CHIP_INFO	_IOW(GF_IOC_MAGIC, 13, struct gf_ioc_chip_info)
#define GF_IOC_NAV_EVENT	_IOW(GF_IOC_MAGIC, 14, gf_nav_event_t)
#define GF_IOC_MAXNR		15

#define NETLINK_GOODIX		25
#define GF_NET_EVENT_IRQ	1
#define GF_NET_EVENT_FB_BLACK	2
#define GF_NET_EVENT_FB_UNBLACK	3

#endif /* _UAPI_LINUX_GOODIX_FP_H */
