/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_P61_ESE_H
#define _UAPI_LINUX_P61_ESE_H

#include <linux/ioctl.h>

#define P61_MAGIC		0xEA
#define P61_SET_PWR		_IOW(P61_MAGIC, 0x01, long)
#define P61_SET_DBG		_IOW(P61_MAGIC, 0x02, long)
#define P61_SET_POLL		_IOW(P61_MAGIC, 0x03, long)
#define P61_SET_SPM_PWR		_IOW(P61_MAGIC, 0x04, long)
#define P61_GET_SPM_STATUS	_IOR(P61_MAGIC, 0x05, long)
#define P61_SET_THROUGHPUT	_IOW(P61_MAGIC, 0x06, long)
#define P61_GET_ESE_ACCESS	_IOW(P61_MAGIC, 0x07, long)
#define P61_SET_POWER_SCHEME	_IOW(P61_MAGIC, 0x08, long)
#define P61_SET_DWNLD_STATUS	_IOW(P61_MAGIC, 0x09, long)
#define P61_INHIBIT_PWR_CNTRL	_IOW(P61_MAGIC, 0x0A, long)

#endif /* _UAPI_LINUX_P61_ESE_H */
