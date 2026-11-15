/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __QSEECOM_KERNEL_H_
#define __QSEECOM_KERNEL_H_

#include <linux/types.h>

#define QSEECOM_ALIGN_SIZE	0x40
#define QSEECOM_ALIGN_MASK	(QSEECOM_ALIGN_SIZE - 1)
#define QSEECOM_ALIGN(x)	(((x) + QSEECOM_ALIGN_MASK) & ~QSEECOM_ALIGN_MASK)

struct qseecom_handle {
	void *dev;
	unsigned char *sbuf;
	u32 sbuf_len;
};

int qseecom_start_app(struct qseecom_handle **handle, char *app_name, u32 size);
int qseecom_shutdown_app(struct qseecom_handle **handle);
int qseecom_send_command(struct qseecom_handle *handle, void *send_buf,
			 u32 sbuf_len, void *resp_buf, u32 rbuf_len);

#endif /* __QSEECOM_KERNEL_H_ */
