// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm TrustZone Secure Execution Environment Communicator (/dev/qseecom)
 * Modernized Linux 6.18 port using upstream qcom_scm, dma-buf, and CMA.
 *
 * Copyright (c) 2012-2019, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt) "qseecom: %s: " fmt, __func__

#include <linux/cleanup.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/elf.h>
#include <linux/firmware.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/fs.h>
#include <linux/iosys-map.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <soc/qcom/qseecomi.h>
#include <uapi/linux/qseecom.h>

#include "qseecom_kernel.h"

#define QSEECOM_DEV_NAME		"qseecom"
#define QSEE_VERSION_02			0x402000
#define QSEE_VERSION_40			0x1000000
#define QSEOS_VERSION_14		0x14

#define FEATURE_ID_QSEE_VERSION		10
#define FEATURE_ID_WHITELIST		15
#define WHITELIST_V1_0_0		BIT(22)

#define QSEECOM_SCM_EBUSY_WAIT_MS	30
#define QSEECOM_SCM_EBUSY_MAX_RETRY	20
#define PHY_ADDR_4G			0x100000000ULL

#define SGLISTINFO_SET_INDEX_FLAG(contig, is64, off) \
	((u32)((((contig) & 1U) << 31) | (((is64) & 1U) << 30) | ((off) & 0x3fffffffU)))

struct sglist_info {
	u32 index_and_flags;
	u32 size_or_count;
} __packed;

#define SGLISTINFO_TABLE_SIZE		(sizeof(struct sglist_info) * MAX_ION_FD)

enum qseecom_client_handle_type {
	QSEECOM_CLIENT_APP = 1,
	QSEECOM_LISTENER_SERVICE,
	QSEECOM_SECURE_SERVICE,
	QSEECOM_GENERIC,
	QSEECOM_UNAVAILABLE_CLIENT_APP,
};

struct qseecom_dmabuf_map {
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct iosys_map vmap;
	bool vmap_valid;
	phys_addr_t paddr;
	size_t len;
};

struct qseecom_listener_entry {
	struct list_head list;
	struct qseecom_register_listener_req svc;
	struct qseecom_dmabuf_map sb;
	void *user_virt_sb_base;
	wait_queue_head_t rcv_req_wq;
	wait_queue_head_t block_app_wq;
	int rcv_req_flag;
	int send_resp_flag;
	bool listener_in_use;
	struct sglist_info *sglistinfo;
	dma_addr_t sglistinfo_dma;
	u32 sglist_cnt;
	int abort;
	bool unregister_pending;
};

struct qseecom_app_entry {
	struct list_head list;
	u32 app_id;
	u32 ref_cnt;
	char app_name[MAX_APP_NAME_SIZE];
	u32 app_arch;
	bool app_blocked;
	u32 check_block;
	u32 blocked_on_listener_id;
};

struct qseecom_dev_handle;

struct qseecom_pending_lsnr_unreg {
	struct list_head list;
	struct qseecom_dev_handle *data;
};

struct qseecom_pending_app_unload {
	struct list_head list;
	struct qseecom_dev_handle *data;
};

struct qseecom_client_data {
	u32 app_id;
	char app_name[MAX_APP_NAME_SIZE];
	u32 app_arch;
	struct qseecom_dmabuf_map sb;
	void *user_virt_sb_base;
	bool unload_pending;
	void *kbuf_virt;
	dma_addr_t kbuf_dma;
	size_t kbuf_size;
};

struct qseecom_listener_data {
	u32 id;
	bool release_called;
};

struct qseecom_dev_handle {
	enum qseecom_client_handle_type type;
	struct qseecom_client_data client;
	struct qseecom_listener_data listener;
	struct sglist_info *sglistinfo;
	dma_addr_t sglistinfo_dma;
	u32 sglist_cnt;
	int abort;
	bool released;
	bool use_legacy_cmd;
	wait_queue_head_t abort_wq;
	atomic_t ioctl_count;
};

struct qseecom_control {
	struct device *dev;
	u32 qsee_version;
	u32 qseos_version;
	bool appsbl_qseecom_support;
	bool commonlib_loaded;
	bool commonlib64_loaded;
	bool no_clock_support;
	bool whitelist_support;
	bool smcinvoke_support;
	u32 reentrancy_support;
	int is_apps_region_protected;

	struct list_head registered_listener_list_head;
	struct list_head registered_app_list_head;
	spinlock_t registered_app_list_lock;

	wait_queue_head_t send_resp_wq;
	int send_resp_flag;
	u32 app_block_ref_cnt;
	wait_queue_head_t app_block_wq;

	struct list_head unregister_lsnr_pending_head;
	wait_queue_head_t register_lsnr_pending_wq;
	struct task_struct *unreg_lsnr_kthread;
	wait_queue_head_t unreg_lsnr_kthread_wq;
	atomic_t unreg_lsnr_state;

	struct list_head unload_app_pending_head;
	struct task_struct *unload_app_kthread;
	wait_queue_head_t unload_app_kthread_wq;
	atomic_t unload_app_state;
};

static struct qseecom_control qseecom;
static DEFINE_MUTEX(app_access_lock);
static DEFINE_MUTEX(listener_access_lock);
static DEFINE_MUTEX(unload_app_pending_lock);

static int __init qseecom_keymaster_status_setup(char *str)
{
	get_option(&str, &qseecom.is_apps_region_protected);
	return 1;
}
__setup("androidboot.keymaster=", qseecom_keymaster_status_setup);

/* ---------- DMA-BUF helpers ---------- */

static void qseecom_dmabuf_unmap(struct qseecom_dmabuf_map *m)
{
	if (!m || !m->dmabuf)
		return;

	if (m->vmap_valid) {
		dma_buf_vunmap_unlocked(m->dmabuf, &m->vmap);
		m->vmap_valid = false;
	}
	if (m->sgt && m->attach) {
		dma_buf_unmap_attachment_unlocked(m->attach, m->sgt,
						  DMA_BIDIRECTIONAL);
		m->sgt = NULL;
	}
	if (m->attach) {
		dma_buf_detach(m->dmabuf, m->attach);
		m->attach = NULL;
	}
	dma_buf_put(m->dmabuf);
	memset(m, 0, sizeof(*m));
}

static int qseecom_dmabuf_map(int fd, bool need_vmap, bool require_contig,
			      struct qseecom_dmabuf_map *m)
{
	struct scatterlist *sg;
	int ret;

	memset(m, 0, sizeof(*m));
	if (fd < 0)
		return -EINVAL;

	m->dmabuf = dma_buf_get(fd);
	if (IS_ERR(m->dmabuf)) {
		ret = PTR_ERR(m->dmabuf);
		m->dmabuf = NULL;
		return ret;
	}

	m->attach = dma_buf_attach(m->dmabuf, qseecom.dev);
	if (IS_ERR(m->attach)) {
		ret = PTR_ERR(m->attach);
		m->attach = NULL;
		goto err;
	}

	m->sgt = dma_buf_map_attachment_unlocked(m->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(m->sgt)) {
		ret = PTR_ERR(m->sgt);
		m->sgt = NULL;
		goto err;
	}

	if (!m->sgt->nents) {
		ret = -EINVAL;
		goto err;
	}

	if (require_contig && m->sgt->nents != 1) {
		pr_err("buffer must be physically contiguous (nents=%u)\n",
		       m->sgt->nents);
		ret = -EINVAL;
		goto err;
	}

	m->paddr = sg_dma_address(m->sgt->sgl);
	m->len = 0;
	{
		unsigned int idx;

		for_each_sgtable_dma_sg(m->sgt, sg, idx)
			m->len += sg_dma_len(sg);
	}

	if (need_vmap) {
		ret = dma_buf_vmap_unlocked(m->dmabuf, &m->vmap);
		if (ret || iosys_map_is_null(&m->vmap)) {
			ret = ret ? ret : -ENOMEM;
			goto err;
		}
		m->vmap_valid = true;
	}

	return 0;

err:
	qseecom_dmabuf_unmap(m);
	return ret;
}

static int qseecom_dmabuf_sync(struct qseecom_dmabuf_map *m, bool to_device)
{
	if (!m || !m->dmabuf)
		return 0;
	return to_device
		? dma_buf_end_cpu_access(m->dmabuf, DMA_BIDIRECTIONAL)
		: dma_buf_begin_cpu_access(m->dmabuf, DMA_BIDIRECTIONAL);
}

static inline void *qseecom_sb_kvirt(struct qseecom_dmabuf_map *sb)
{
	return sb->vmap.vaddr;
}

/* ---------- SCM bridge ---------- */

static int qseecom_scm_exec_locked(u32 owner, u32 svc, u32 cmd, u32 arginfo,
				   const u64 *args, size_t nargs,
				   struct qseecom_command_scm_resp *resp)
{
	struct qcom_scm_qseecom_resp res = {};
	int retry = 0;
	int ret;

	do {
		ret = qcom_scm_qseecom_raw(owner, svc, cmd, arginfo,
					   args, nargs, &res);
		if (ret == -EBUSY) {
			mutex_unlock(&app_access_lock);
			msleep(QSEECOM_SCM_EBUSY_WAIT_MS);
			mutex_lock(&app_access_lock);
		}
	} while (ret == -EBUSY && retry++ < QSEECOM_SCM_EBUSY_MAX_RETRY);

	if (resp) {
		resp->result = lower_32_bits(res.result);
		resp->resp_type = lower_32_bits(res.resp_type);
		resp->data = lower_32_bits(res.data);
	}
	return ret;
}

static int qseecom_get_feature_version(u32 feature, u32 *version)
{
	struct qcom_scm_qseecom_resp res = {};
	u64 args[1] = { feature };
	int ret;

	ret = qcom_scm_qseecom_raw(TZ_OWNER_SIP, TZ_SVC_INFO, 3,
				   TZ_ARGS_1(TZ_PARAM_VAL), args, 1, &res);
	if (!ret && version)
		*version = lower_32_bits(res.result);
	return ret;
}

static int qseecom_lookup_app_id(const char *app_name, u32 *app_id)
{
	struct qseecom_command_scm_resp resp = {};
	phys_addr_t phys = 0;
	char *tzbuf;
	size_t len;
	u64 args[2];
	int ret;

	len = strnlen(app_name, MAX_APP_NAME_SIZE);
	if (!len || len >= MAX_APP_NAME_SIZE)
		return -EINVAL;

	tzbuf = qcom_scm_tzmem_alloc(PAGE_ALIGN(MAX_APP_NAME_SIZE),
				     GFP_KERNEL, &phys);
	if (!tzbuf)
		return -ENOMEM;

	strscpy(tzbuf, app_name, MAX_APP_NAME_SIZE);
	args[0] = phys;
	args[1] = len;

	ret = qseecom_scm_exec_locked(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 3,
				      TZ_ARGS_2(TZ_PARAM_BUF_RW, TZ_PARAM_VAL),
				      args, 2, &resp);
	qcom_scm_tzmem_free(tzbuf);

	if (ret)
		return ret;

	if (resp.result == QSEOS_RESULT_FAILURE) {
		*app_id = 0;
		return 0;
	}
	if (resp.result != QSEOS_RESULT_SUCCESS)
		return -EINVAL;

	*app_id = resp.data;
	return 0;
}

/* ---------- Registry helpers ---------- */

static struct qseecom_listener_entry *qseecom_find_svc(u32 listener_id)
{
	struct qseecom_listener_entry *entry;

	list_for_each_entry(entry, &qseecom.registered_listener_list_head, list) {
		if (entry->svc.listener_id == listener_id)
			return entry;
	}
	return NULL;
}

static struct qseecom_app_entry *qseecom_find_app_entry(u32 app_id,
							const char *name)
{
	struct qseecom_app_entry *entry;
	unsigned long flags;

	spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
	list_for_each_entry(entry, &qseecom.registered_app_list_head, list) {
		if (entry->app_id == app_id &&
		    (!name || !strcmp(entry->app_name, name))) {
			spin_unlock_irqrestore(&qseecom.registered_app_list_lock,
					       flags);
			return entry;
		}
	}
	spin_unlock_irqrestore(&qseecom.registered_app_list_lock, flags);
	return NULL;
}

static void qseecom_add_app_entry(u32 app_id, const char *name, u32 app_arch)
{
	struct qseecom_app_entry *entry;
	unsigned long flags;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return;

	entry->app_id = app_id;
	entry->ref_cnt = 1;
	entry->app_arch = app_arch;
	if (!strcmp(name, "keymaste"))
		strscpy(entry->app_name, "keymaster", MAX_APP_NAME_SIZE);
	else
		strscpy(entry->app_name, name, MAX_APP_NAME_SIZE);

	spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
	list_add_tail(&entry->list, &qseecom.registered_app_list_head);
	spin_unlock_irqrestore(&qseecom.registered_app_list_lock, flags);
}

static void qseecom_normalize_app_name(char *dst, const char *src)
{
	if (!strcmp(src, "keymaste"))
		strscpy(dst, "keymaster", MAX_APP_NAME_SIZE);
	else
		strscpy(dst, src, MAX_APP_NAME_SIZE);
}

/* ---------- Listener request/response & reentrancy ---------- */

static int qseecom_send_listener_resp_scm(u32 lstnr, u32 status,
					  struct qseecom_listener_entry *svc,
					  struct qseecom_command_scm_resp *resp)
{
	u64 args[4] = { lstnr, status, 0, 0 };

	if (svc && svc->sglist_cnt && qseecom.whitelist_support) {
		args[2] = svc->sglistinfo_dma;
		args[3] = SGLISTINFO_TABLE_SIZE;
		return qseecom_scm_exec_locked(
			TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 5,
			TZ_ARGS_4(TZ_PARAM_VAL, TZ_PARAM_VAL,
				  TZ_PARAM_BUF_RW, TZ_PARAM_VAL),
			args, 4, resp);
	}

	return qseecom_scm_exec_locked(
		TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 3,
		TZ_ARGS_2(TZ_PARAM_VAL, TZ_PARAM_VAL), args, 2, resp);
}

static int qseecom_process_incomplete_cmd(struct qseecom_dev_handle *data,
					  struct qseecom_command_scm_resp *resp)
{
	struct qseecom_listener_entry *ptr_svc = NULL;
	sigset_t new_sig, old_sig;
	u32 lstnr, status;
	int ret = 0, rc = 0;

	if (!qseecom.reentrancy_support)
		qseecom.app_block_ref_cnt++;

	while (ret == 0 && resp->result == QSEOS_RESULT_INCOMPLETE) {
		lstnr = resp->data;
		mutex_lock(&listener_access_lock);
		ptr_svc = qseecom_find_svc(lstnr);
		if (!ptr_svc || !ptr_svc->sb.dmabuf || ptr_svc->abort) {
			pr_err("listener %u invalid or aborted\n", lstnr);
			rc = ptr_svc && ptr_svc->abort ? -ENODEV : -EINVAL;
			status = QSEOS_RESULT_FAILURE;
			goto send_err_rsp;
		}

		ptr_svc->listener_in_use = true;
		qseecom_dmabuf_sync(&ptr_svc->sb, false);
		ptr_svc->rcv_req_flag = 1;
		wake_up_interruptible(&ptr_svc->rcv_req_wq);

		sigfillset(&new_sig);
		sigprocmask(SIG_SETMASK, &new_sig, &old_sig);
		mutex_unlock(&listener_access_lock);

		do {
			if (!qseecom.reentrancy_support) {
				if (!wait_event_interruptible(
					qseecom.send_resp_wq,
					qseecom.send_resp_flag || data->abort ||
					ptr_svc->abort))
					break;
			} else {
				if (!wait_event_interruptible(
					qseecom.send_resp_wq,
					ptr_svc->send_resp_flag || data->abort ||
					ptr_svc->abort))
					break;
			}
		} while (1);

		mutex_lock(&listener_access_lock);
		sigprocmask(SIG_SETMASK, &old_sig, NULL);

		if (data->abort || ptr_svc->abort) {
			rc = -ENODEV;
			status = QSEOS_RESULT_FAILURE;
		} else {
			qseecom_dmabuf_sync(&ptr_svc->sb, true);
			status = QSEOS_RESULT_SUCCESS;
		}

send_err_rsp:
		qseecom.send_resp_flag = 0;
		if (ptr_svc)
			ptr_svc->send_resp_flag = 0;

		ret = qseecom_send_listener_resp_scm(lstnr, status, ptr_svc, resp);
		if (ptr_svc) {
			if (ptr_svc->sglist_cnt) {
				memset(ptr_svc->sglistinfo, 0, SGLISTINFO_TABLE_SIZE);
				ptr_svc->sglist_cnt = 0;
			}
			ptr_svc->listener_in_use = false;
			wake_up_interruptible_all(&ptr_svc->block_app_wq);
		}
		mutex_unlock(&listener_access_lock);
	}

	if (!qseecom.reentrancy_support) {
		qseecom.app_block_ref_cnt--;
		wake_up_interruptible_all(&qseecom.app_block_wq);
	}

	return rc ? rc : ret;
}

static int qseecom_continue_blocked(struct qseecom_dev_handle *data,
				    struct qseecom_app_entry *ptr_app,
				    struct qseecom_command_scm_resp *resp)
{
	struct qseecom_listener_entry *list_ptr;
	sigset_t new_sig, old_sig;
	int ret = 0;

	if (!ptr_app)
		ptr_app = qseecom_find_app_entry(data->client.app_id,
						 data->client.app_name);
	if (!ptr_app)
		return -ENOENT;

	do {
		u32 session_id = resp->resp_type;
		u64 args[1];

		mutex_lock(&listener_access_lock);
		list_ptr = qseecom_find_svc(resp->data);
		if (!list_ptr) {
			mutex_unlock(&listener_access_lock);
			return -ENODATA;
		}
		ptr_app->blocked_on_listener_id = resp->data;

		sigfillset(&new_sig);
		sigprocmask(SIG_SETMASK, &new_sig, &old_sig);
		while (list_ptr->listener_in_use) {
			qseecom.app_block_ref_cnt++;
			ptr_app->app_blocked = true;
			mutex_unlock(&listener_access_lock);
			mutex_unlock(&app_access_lock);
			wait_event_interruptible(list_ptr->block_app_wq,
						 !list_ptr->listener_in_use ||
						 data->abort);
			mutex_lock(&app_access_lock);
			mutex_lock(&listener_access_lock);
			ptr_app->app_blocked = false;
			qseecom.app_block_ref_cnt--;
			if (data->abort) {
				sigprocmask(SIG_SETMASK, &old_sig, NULL);
				mutex_unlock(&listener_access_lock);
				return -ENODEV;
			}
		}
		sigprocmask(SIG_SETMASK, &old_sig, NULL);
		ptr_app->blocked_on_listener_id = 0;

		args[0] = qseecom.smcinvoke_support ? session_id : data->client.app_id;
		ret = qseecom_scm_exec_locked(
			TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER,
			qseecom.smcinvoke_support ? 7 : 4,
			TZ_ARGS_1(TZ_PARAM_VAL), args, 1, resp);
		if (ret && qseecom.smcinvoke_support) {
			args[0] = data->client.app_id;
			ret = qseecom_scm_exec_locked(
				TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 4,
				TZ_ARGS_1(TZ_PARAM_VAL), args, 1, resp);
		}
		mutex_unlock(&listener_access_lock);
		if (ret)
			return ret;
	} while (resp->result == QSEOS_RESULT_BLOCKED_ON_LISTENER);

	return (resp->result == QSEOS_RESULT_INCOMPLETE) ? 0 : -EINVAL;
}

static int qseecom_handle_tz_resp(struct qseecom_dev_handle *data,
				  struct qseecom_app_entry *ptr_app,
				  struct qseecom_command_scm_resp *resp)
{
	int ret = 0;

	if (!qseecom.reentrancy_support) {
		if (resp->result == QSEOS_RESULT_INCOMPLETE)
			return qseecom_process_incomplete_cmd(data, resp);
		return (resp->result == QSEOS_RESULT_SUCCESS) ? 0 : -EINVAL;
	}

	if (resp->result == QSEOS_RESULT_BLOCKED_ON_LISTENER) {
		ret = qseecom_continue_blocked(data, ptr_app, resp);
		wake_up_interruptible_all(&qseecom.app_block_wq);
		if (ret)
			return ret;
	}

	if (resp->result == QSEOS_RESULT_INCOMPLETE) {
		ret = qseecom_process_incomplete_cmd(data, resp);
		wake_up_interruptible_all(&qseecom.app_block_wq);
		return ret;
	}

	return (resp->result == QSEOS_RESULT_SUCCESS) ? 0 : -EINVAL;
}

/* ---------- Firmware staging helpers ---------- */

static int qseecom_read_split_fw(const char *appname, void **buf_out,
				 dma_addr_t *dma_out, u32 *mdt_len,
				 u32 *img_len, u32 *app_arch)
{
	const struct firmware *fw = NULL;
	char fw_name[MAX_APP_NAME_SIZE];
	u32 total = 0, arch, n_phdrs, i;
	void *buf = NULL;
	dma_addr_t dma = 0;
	u8 *cursor;
	int ret;

	snprintf(fw_name, sizeof(fw_name), "%s.mdt", appname);
	ret = request_firmware(&fw, fw_name, qseecom.dev);
	if (ret)
		return ret;

	if (fw->size < EI_CLASS + 1) {
		release_firmware(fw);
		return -EINVAL;
	}
	arch = fw->data[EI_CLASS];
	if (arch == ELFCLASS32 && fw->size >= sizeof(struct elf32_hdr))
		n_phdrs = ((const struct elf32_hdr *)fw->data)->e_phnum;
	else if (arch == ELFCLASS64 && fw->size >= sizeof(struct elf64_hdr))
		n_phdrs = ((const struct elf64_hdr *)fw->data)->e_phnum;
	else {
		release_firmware(fw);
		return -EINVAL;
	}

	*mdt_len = fw->size;
	total = fw->size;
	release_firmware(fw);
	fw = NULL;

	for (i = 0; i < n_phdrs; i++) {
		snprintf(fw_name, sizeof(fw_name), "%s.b%02u", appname, i);
		ret = request_firmware(&fw, fw_name, qseecom.dev);
		if (ret)
			return ret;
		if (total > U32_MAX - fw->size) {
			release_firmware(fw);
			return -EINVAL;
		}
		total += fw->size;
		release_firmware(fw);
		fw = NULL;
	}

	buf = dma_alloc_coherent(qseecom.dev, PAGE_ALIGN(total), &dma, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	cursor = buf;

	snprintf(fw_name, sizeof(fw_name), "%s.mdt", appname);
	ret = request_firmware(&fw, fw_name, qseecom.dev);
	if (ret)
		goto err;
	memcpy(cursor, fw->data, fw->size);
	cursor += fw->size;
	release_firmware(fw);
	fw = NULL;

	for (i = 0; i < n_phdrs; i++) {
		snprintf(fw_name, sizeof(fw_name), "%s.b%02u", appname, i);
		ret = request_firmware(&fw, fw_name, qseecom.dev);
		if (ret)
			goto err;
		memcpy(cursor, fw->data, fw->size);
		cursor += fw->size;
		release_firmware(fw);
		fw = NULL;
	}

	*buf_out = buf;
	*dma_out = dma;
	*img_len = total;
	if (app_arch)
		*app_arch = arch;
	return 0;

err:
	if (fw)
		release_firmware(fw);
	dma_free_coherent(qseecom.dev, PAGE_ALIGN(total), buf, dma);
	return ret;
}

static int qseecom_load_commonlib_image(struct qseecom_dev_handle *data,
					const char *name)
{
	struct qseecom_command_scm_resp resp = {};
	u32 mdt_len = 0, img_len = 0;
	dma_addr_t dma = 0;
	void *buf = NULL;
	u64 args[3];
	int ret;

	ret = qseecom_read_split_fw(name, &buf, &dma, &mdt_len, &img_len, NULL);
	if (ret)
		return ret;

	args[0] = mdt_len;
	args[1] = img_len;
	args[2] = dma;

	ret = qseecom_scm_exec_locked(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 7,
				      TZ_ARGS_3(TZ_PARAM_VAL, TZ_PARAM_VAL,
						TZ_PARAM_VAL),
				      args, 3, &resp);
	if (!ret)
		ret = qseecom_handle_tz_resp(data, NULL, &resp);

	dma_free_coherent(qseecom.dev, PAGE_ALIGN(img_len), buf, dma);
	return ret;
}

/* ---------- Core IOCTL handlers ---------- */

static int qseecom_get_qseos_version(void __user *argp)
{
	struct qseecom_qseos_version_req req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	req.qseos_version = qseecom.qseos_version;
	return copy_to_user(argp, &req, sizeof(req)) ? -EFAULT : 0;
}

static int qseecom_app_load_query(struct qseecom_dev_handle *data,
				  void __user *argp)
{
	struct qseecom_qseos_app_load_query req;
	struct qseecom_app_entry *entry;
	u32 app_id = 0;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	req.app_name[MAX_APP_NAME_SIZE - 1] = 0;

	ret = qseecom_lookup_app_id(req.app_name, &app_id);
	if (ret)
		return ret;

	req.app_id = app_id;
	if (!app_id)
		return copy_to_user(argp, &req, sizeof(req)) ? -EFAULT : -ENOENT;

	qseecom_normalize_app_name(data->client.app_name, req.app_name);
	entry = qseecom_find_app_entry(app_id, data->client.app_name);
	if (!entry)
		qseecom_add_app_entry(app_id, data->client.app_name, req.app_arch);
	else
		entry->ref_cnt++;

	data->client.app_id = app_id;
	return copy_to_user(argp, &req, sizeof(req)) ? -EFAULT : 0;
}

static int qseecom_set_client_mem_param(struct qseecom_dev_handle *data,
					void __user *argp)
{
	struct qseecom_set_sb_mem_param_req req;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.ifd_data_fd < 0 || !req.virt_sb_base || !req.sb_len)
		return -EINVAL;
	if (!access_ok((void __user *)req.virt_sb_base, req.sb_len))
		return -EFAULT;

	qseecom_dmabuf_unmap(&data->client.sb);
	ret = qseecom_dmabuf_map(req.ifd_data_fd, true, true, &data->client.sb);
	if (ret)
		return ret;

	if (data->client.sb.len < req.sb_len) {
		qseecom_dmabuf_unmap(&data->client.sb);
		return -EINVAL;
	}

	data->client.sb.len = req.sb_len;
	data->client.user_virt_sb_base = req.virt_sb_base;
	return 0;
}

static int qseecom_load_app(struct qseecom_dev_handle *data, void __user *argp)
{
	struct qseecom_load_img_req req;
	struct qseecom_command_scm_resp resp = {};
	struct qseecom_dmabuf_map fw_buf = {};
	struct qseecom_app_entry *entry;
	u32 app_id = 0;
	u64 args[3];
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	req.img_name[MAX_APP_NAME_SIZE - 1] = 0;

	if (!qseecom.commonlib_loaded && req.app_arch == ELFCLASS32) {
		ret = qseecom_load_commonlib_image(data, "cmnlib");
		if (ret)
			return ret;
		qseecom.commonlib_loaded = true;
	}
	if (!qseecom.commonlib64_loaded && req.app_arch == ELFCLASS64) {
		ret = qseecom_load_commonlib_image(data, "cmnlib64");
		if (ret)
			return ret;
		qseecom.commonlib64_loaded = true;
	}

	ret = qseecom_lookup_app_id(req.img_name, &app_id);
	if (ret)
		return ret;

	qseecom_normalize_app_name(data->client.app_name, req.img_name);
	data->client.app_arch = req.app_arch;

	if (app_id) {
		entry = qseecom_find_app_entry(app_id, data->client.app_name);
		if (entry)
			entry->ref_cnt++;
		else
			qseecom_add_app_entry(app_id, req.img_name, req.app_arch);
		data->client.app_id = app_id;
		req.app_id = app_id;
		return copy_to_user(argp, &req, sizeof(req)) ? -EFAULT : 0;
	}

	ret = qseecom_dmabuf_map(req.ifd_data_fd, false, true, &fw_buf);
	if (ret)
		return ret;

	if (req.mdt_len > fw_buf.len || req.img_len > fw_buf.len) {
		qseecom_dmabuf_unmap(&fw_buf);
		return -EINVAL;
	}

	qseecom_dmabuf_sync(&fw_buf, true);
	args[0] = req.mdt_len;
	args[1] = req.img_len;
	args[2] = fw_buf.paddr;

	ret = qseecom_scm_exec_locked(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 1,
				      TZ_ARGS_3(TZ_PARAM_VAL, TZ_PARAM_VAL,
						TZ_PARAM_VAL),
				      args, 3, &resp);
	if (!ret) {
		if (resp.result == QSEOS_RESULT_INCOMPLETE)
			ret = qseecom_process_incomplete_cmd(data, &resp);
		else if (resp.result != QSEOS_RESULT_SUCCESS)
			ret = -EFAULT;
	}
	qseecom_dmabuf_unmap(&fw_buf);
	if (ret)
		return ret;

	app_id = resp.data;
	qseecom_add_app_entry(app_id, req.img_name, req.app_arch);
	data->client.app_id = app_id;
	req.app_id = app_id;
	pr_info("loaded app %s (id=%u)\n", req.img_name, app_id);
	return copy_to_user(argp, &req, sizeof(req)) ? -EFAULT : 0;
}

static int __qseecom_unload_app_id(struct qseecom_dev_handle *data, u32 app_id)
{
	struct qseecom_command_scm_resp resp = {};
	u64 args[1] = { app_id };
	int ret;

	ret = qseecom_scm_exec_locked(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 2,
				      TZ_ARGS_1(TZ_PARAM_VAL), args, 1, &resp);
	if (ret)
		return ret;
	if (resp.result == QSEOS_RESULT_INCOMPLETE)
		return qseecom_process_incomplete_cmd(data, &resp);
	return (resp.result == QSEOS_RESULT_SUCCESS) ? 0 : -EFAULT;
}

static int qseecom_unload_app(struct qseecom_dev_handle *data, bool app_crash)
{
	struct qseecom_app_entry *entry;
	unsigned long flags;
	bool unload = false;
	int ret = 0;

	if (!strncmp(data->client.app_name, "keymaste", 8))
		goto done;

	entry = qseecom_find_app_entry(data->client.app_id,
				       data->client.app_name);
	if (!entry)
		goto done;

	if (entry->app_blocked || entry->check_block)
		app_crash = false;
	if (app_crash || entry->ref_cnt <= 1)
		unload = true;

	if (unload) {
		ret = __qseecom_unload_app_id(data, data->client.app_id);
		spin_lock_irqsave(&qseecom.registered_app_list_lock, flags);
		list_del(&entry->list);
		spin_unlock_irqrestore(&qseecom.registered_app_list_lock, flags);
		kfree(entry);
	} else {
		entry->ref_cnt--;
	}

done:
	qseecom_dmabuf_unmap(&data->client.sb);
	data->released = true;
	return ret;
}

static int qseecom_register_listener(struct qseecom_dev_handle *data,
				     void __user *argp)
{
	struct qseecom_register_listener_req rcvd;
	struct qseecom_listener_entry *entry;
	struct qseecom_command_scm_resp resp = {};
	u64 args[3];
	int ret;

	if (copy_from_user(&rcvd, argp, sizeof(rcvd)))
		return -EFAULT;
	if (!access_ok((void __user *)rcvd.virt_sb_base, rcvd.sb_size))
		return -EFAULT;

	if (qseecom_find_svc(rcvd.listener_id)) {
		data->released = true;
		return -EBUSY;
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->svc = rcvd;
	entry->user_virt_sb_base = rcvd.virt_sb_base;
	init_waitqueue_head(&entry->rcv_req_wq);
	init_waitqueue_head(&entry->block_app_wq);

	entry->sglistinfo = dma_alloc_coherent(qseecom.dev, PAGE_SIZE,
					       &entry->sglistinfo_dma,
					       GFP_KERNEL);
	if (!entry->sglistinfo) {
		kfree(entry);
		return -ENOMEM;
	}

	ret = qseecom_dmabuf_map(rcvd.ifd_data_fd, true, true, &entry->sb);
	if (ret || entry->sb.len < rcvd.sb_size) {
		qseecom_dmabuf_unmap(&entry->sb);
		dma_free_coherent(qseecom.dev, PAGE_SIZE, entry->sglistinfo,
				  entry->sglistinfo_dma);
		kfree(entry);
		return ret ? ret : -EINVAL;
	}
	entry->sb.len = rcvd.sb_size;

	args[0] = rcvd.listener_id;
	args[1] = entry->sb.paddr;
	args[2] = entry->sb.len;

	mutex_unlock(&listener_access_lock);
	mutex_lock(&app_access_lock);
	qseecom.smcinvoke_support = true;
	ret = qseecom_scm_exec_locked(
		TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 6,
		TZ_ARGS_3(TZ_PARAM_VAL, TZ_PARAM_BUF_RW, TZ_PARAM_VAL),
		args, 3, &resp);
	if (ret == -EIO || ret == -EOPNOTSUPP || ret == -EINVAL) {
		qseecom.smcinvoke_support = false;
		ret = qseecom_scm_exec_locked(
			TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 1,
			TZ_ARGS_3(TZ_PARAM_VAL, TZ_PARAM_BUF_RW, TZ_PARAM_VAL),
			args, 3, &resp);
	}
	mutex_unlock(&app_access_lock);
	mutex_lock(&listener_access_lock);

	if (ret || resp.result != QSEOS_RESULT_SUCCESS) {
		qseecom_dmabuf_unmap(&entry->sb);
		dma_free_coherent(qseecom.dev, PAGE_SIZE, entry->sglistinfo,
				  entry->sglistinfo_dma);
		kfree(entry);
		return ret ? ret : -EPERM;
	}

	list_add_tail(&entry->list, &qseecom.registered_listener_list_head);
	data->listener.id = rcvd.listener_id;
	pr_info("registered listener %u\n", rcvd.listener_id);
	return 0;
}

static int __qseecom_unregister_listener(struct qseecom_dev_handle *data,
					 struct qseecom_listener_entry *svc)
{
	struct qseecom_command_scm_resp resp = {};
	u64 args[1] = { data->listener.id };
	int ret;

	mutex_unlock(&listener_access_lock);
	mutex_lock(&app_access_lock);
	ret = qseecom_scm_exec_locked(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 2,
				      TZ_ARGS_1(TZ_PARAM_VAL), args, 1, &resp);
	mutex_unlock(&app_access_lock);
	mutex_lock(&listener_access_lock);
	if (ret == -EBUSY)
		return ret;

	qseecom_dmabuf_unmap(&svc->sb);
	if (svc->sglistinfo)
		dma_free_coherent(qseecom.dev, PAGE_SIZE, svc->sglistinfo,
				  svc->sglistinfo_dma);
	list_del(&svc->list);
	kfree(svc);
	data->released = true;
	return (ret || resp.result != QSEOS_RESULT_SUCCESS) ? -EPERM : 0;
}

static int qseecom_unregister_listener(struct qseecom_dev_handle *data)
{
	struct qseecom_listener_entry *svc;
	struct qseecom_pending_lsnr_unreg *pending;

	if (data->released)
		return -EINVAL;

	svc = qseecom_find_svc(data->listener.id);
	if (!svc)
		return -ENODATA;

	svc->abort = 1;
	wake_up_interruptible(&svc->rcv_req_wq);
	if (!svc->listener_in_use)
		return __qseecom_unregister_listener(data, svc);

	pending = kzalloc(sizeof(*pending), GFP_KERNEL);
	if (!pending)
		return -ENOMEM;
	pending->data = data;
	list_add_tail(&pending->list, &qseecom.unregister_lsnr_pending_head);
	svc->unregister_pending = true;
	return 0;
}

static int qseecom_receive_req(struct qseecom_dev_handle *data)
{
	struct qseecom_listener_entry *svc;

	mutex_lock(&listener_access_lock);
	svc = qseecom_find_svc(data->listener.id);
	mutex_unlock(&listener_access_lock);
	if (!svc)
		return -ENODATA;

	while (1) {
		if (wait_event_interruptible(svc->rcv_req_wq,
					     svc->rcv_req_flag || data->abort))
			return -ERESTARTSYS;
		if (data->abort)
			return -ENODEV;
		mutex_lock(&listener_access_lock);
		if (svc->rcv_req_flag) {
			svc->rcv_req_flag = 0;
			mutex_unlock(&listener_access_lock);
			return 0;
		}
		mutex_unlock(&listener_access_lock);
	}
}

static int qseecom_validate_client_cmd(struct qseecom_dev_handle *data,
				       const struct qseecom_send_cmd_req *req,
				       uintptr_t *req_off, uintptr_t *rsp_off)
{
	uintptr_t ubase = (uintptr_t)data->client.user_virt_sb_base;
	uintptr_t rq = (uintptr_t)req->cmd_req_buf;
	uintptr_t rs = (uintptr_t)req->resp_buf;
	size_t sblen = data->client.sb.len;

	if (!data->client.sb.dmabuf || !req->cmd_req_len || !req->resp_len)
		return -EINVAL;
	if (rq < ubase || rs < ubase)
		return -EINVAL;
	if (rq - ubase > sblen || req->cmd_req_len > sblen - (rq - ubase))
		return -EINVAL;
	if (rs - ubase > sblen || req->resp_len > sblen - (rs - ubase))
		return -EINVAL;

	*req_off = rq - ubase;
	*rsp_off = rs - ubase;
	return 0;
}

static int qseecom_update_modfd_buf(struct qseecom_dev_handle *data,
				    const struct qseecom_ion_fd_info *fds,
				    void *cmd_ptr, u32 cmd_len,
				    uintptr_t lstnr_sb_off, bool is_64bit,
				    bool cleanup,
				    struct qseecom_listener_entry *svc)
{
	struct sglist_info *sginfo = svc ? svc->sglistinfo : data->sglistinfo;
	u32 *cnt_ptr = svc ? &svc->sglist_cnt : &data->sglist_cnt;
	size_t entry_sz = is_64bit ? SG_ENTRY_SZ_64BIT : SG_ENTRY_SZ;
	int i;

	for (i = 0; i < MAX_ION_FD; i++) {
		struct qseecom_dmabuf_map buf = {};
		struct scatterlist *sg;
		void *field;
		u32 off = fds[i].cmd_buf_offset;
		unsigned int idx;
		int ret;

		if (fds[i].fd <= 0)
			continue;
		if (off > cmd_len || cmd_len - off < sizeof(u32))
			return -EINVAL;

		ret = qseecom_dmabuf_map(fds[i].fd, false, false, &buf);
		if (ret)
			return ret;

		field = (u8 *)cmd_ptr + off;
		if (buf.sgt->nents == 1) {
			u64 pa = cleanup ? 0 : (u64)buf.paddr;

			if (is_64bit) {
				if (cmd_len - off < sizeof(u64)) {
					qseecom_dmabuf_unmap(&buf);
					return -EINVAL;
				}
				*(u64 *)field = pa;
			} else {
				*(u32 *)field = (u32)pa;
			}
		} else {
			if (buf.sgt->nents > (cmd_len - off) / entry_sz) {
				qseecom_dmabuf_unmap(&buf);
				return -EINVAL;
			}
			if (is_64bit) {
				struct qseecom_sg_entry_64bit *e = field;

				for_each_sgtable_dma_sg(buf.sgt, sg, idx) {
					e->phys_addr = cleanup ? 0 : sg_dma_address(sg);
					e->len = cleanup ? 0 : sg_dma_len(sg);
					e++;
				}
			} else {
				struct qseecom_sg_entry *e = field;

				for_each_sgtable_dma_sg(buf.sgt, sg, idx) {
					e->phys_addr = cleanup ? 0 : (u32)sg_dma_address(sg);
					e->len = cleanup ? 0 : sg_dma_len(sg);
					e++;
				}
			}
		}

		qseecom_dmabuf_sync(&buf, !cleanup);
		if (!cleanup && sginfo) {
			u32 sg_off = (u32)(lstnr_sb_off + off);

			sginfo[i].index_and_flags = SGLISTINFO_SET_INDEX_FLAG(
				buf.sgt->nents == 1, 1, sg_off);
			sginfo[i].size_or_count = (buf.sgt->nents == 1)
				? (u32)buf.len : buf.sgt->nents;
			*cnt_ptr = i + 1;
		}
		qseecom_dmabuf_unmap(&buf);
	}

	return 0;
}

static int qseecom_exec_send_cmd(struct qseecom_dev_handle *data,
				 const struct qseecom_send_cmd_req *req)
{
	struct qseecom_command_scm_resp resp = {};
	struct qseecom_app_entry *app;
	uintptr_t req_off = 0, rsp_off = 0;
	u64 req_pa, rsp_pa;
	u64 args[7];
	u32 cmd;
	size_t nargs;
	int ret;

	ret = qseecom_validate_client_cmd(data, req, &req_off, &rsp_off);
	if (ret)
		return ret;

	app = qseecom_find_app_entry(data->client.app_id, data->client.app_name);
	if (!app)
		return -ENOENT;

	req_pa = data->client.sb.paddr + req_off;
	rsp_pa = data->client.sb.paddr + rsp_off;
	if (data->client.app_arch == ELFCLASS32 &&
	    (req_pa >= PHY_ADDR_4G - req->cmd_req_len ||
	     rsp_pa >= PHY_ADDR_4G - req->resp_len))
		return -EFAULT;

	qseecom_dmabuf_sync(&data->client.sb, true);

	args[0] = data->client.app_id;
	args[1] = req_pa;
	args[2] = req->cmd_req_len;
	args[3] = rsp_pa;
	args[4] = req->resp_len;

	if (qseecom.whitelist_support && !data->use_legacy_cmd) {
		cmd = 6;
		args[5] = data->sglistinfo_dma;
		args[6] = SGLISTINFO_TABLE_SIZE;
		nargs = 7;
		ret = qseecom_scm_exec_locked(
			TZ_OWNER_TZ_APPS, TZ_SVC_APP_ID_PLACEHOLDER, cmd,
			TZ_ARGS_7(TZ_PARAM_VAL, TZ_PARAM_BUF_RW, TZ_PARAM_VAL,
				  TZ_PARAM_BUF_RW, TZ_PARAM_VAL, TZ_PARAM_BUF_RW,
				  TZ_PARAM_VAL),
			args, nargs, &resp);
	} else {
		cmd = 1;
		nargs = 5;
		ret = qseecom_scm_exec_locked(
			TZ_OWNER_TZ_APPS, TZ_SVC_APP_ID_PLACEHOLDER, cmd,
			TZ_ARGS_5(TZ_PARAM_VAL, TZ_PARAM_BUF_RW, TZ_PARAM_VAL,
				  TZ_PARAM_BUF_RW, TZ_PARAM_VAL),
			args, nargs, &resp);
	}

	if (!ret)
		ret = qseecom_handle_tz_resp(data, app, &resp);

	qseecom_dmabuf_sync(&data->client.sb, false);
	if (data->sglist_cnt) {
		memset(data->sglistinfo, 0, SGLISTINFO_TABLE_SIZE);
		data->sglist_cnt = 0;
	}
	return ret;
}

static int qseecom_send_cmd_ioctl(struct qseecom_dev_handle *data,
				  void __user *argp)
{
	struct qseecom_send_cmd_req req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	return qseecom_exec_send_cmd(data, &req);
}

static int qseecom_send_modfd_cmd_ioctl(struct qseecom_dev_handle *data,
					void __user *argp, bool is_64bit)
{
	struct qseecom_send_modfd_cmd_req req;
	struct qseecom_send_cmd_req base;
	void *kcmd;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	base.cmd_req_buf = req.cmd_req_buf;
	base.cmd_req_len = req.cmd_req_len;
	base.resp_buf = req.resp_buf;
	base.resp_len = req.resp_len;

	if (!data->client.sb.dmabuf ||
	    (uintptr_t)req.cmd_req_buf < (uintptr_t)data->client.user_virt_sb_base)
		return -EINVAL;

	kcmd = (u8 *)qseecom_sb_kvirt(&data->client.sb) +
	       ((uintptr_t)req.cmd_req_buf -
		(uintptr_t)data->client.user_virt_sb_base);

	ret = qseecom_update_modfd_buf(data, req.ifd_data, kcmd,
				       req.cmd_req_len, 0, is_64bit,
				       false, NULL);
	if (ret)
		return ret;

	ret = qseecom_exec_send_cmd(data, &base);
	qseecom_update_modfd_buf(data, req.ifd_data, kcmd, req.cmd_req_len,
				 0, is_64bit, true, NULL);
	return ret;
}

static int qseecom_send_resp(struct qseecom_dev_handle *data)
{
	struct qseecom_listener_entry *svc;

	mutex_lock(&listener_access_lock);
	svc = qseecom_find_svc(data->listener.id);
	if (!svc) {
		mutex_unlock(&listener_access_lock);
		return -EINVAL;
	}
	qseecom.send_resp_flag = 1;
	svc->send_resp_flag = 1;
	wake_up_interruptible(&qseecom.send_resp_wq);
	mutex_unlock(&listener_access_lock);
	return 0;
}

static int qseecom_send_modfd_resp(struct qseecom_dev_handle *data,
				   void __user *argp, bool is_64bit)
{
	struct qseecom_send_modfd_listener_resp resp;
	struct qseecom_listener_entry *svc;
	uintptr_t sb_off;
	int ret;

	if (copy_from_user(&resp, argp, sizeof(resp)))
		return -EFAULT;

	mutex_lock(&listener_access_lock);
	svc = qseecom_find_svc(data->listener.id);
	if (!svc || !svc->sb.dmabuf || !resp.resp_len ||
	    resp.resp_len > svc->sb.len) {
		mutex_unlock(&listener_access_lock);
		return -EINVAL;
	}

	if ((uintptr_t)resp.resp_buf_ptr < (uintptr_t)svc->user_virt_sb_base) {
		mutex_unlock(&listener_access_lock);
		return -EINVAL;
	}
	sb_off = (uintptr_t)resp.resp_buf_ptr - (uintptr_t)svc->user_virt_sb_base;
	if (sb_off > svc->sb.len || resp.resp_len > svc->sb.len - sb_off) {
		mutex_unlock(&listener_access_lock);
		return -EINVAL;
	}

	ret = qseecom_update_modfd_buf(data, resp.ifd_data,
				       (u8 *)qseecom_sb_kvirt(&svc->sb) + sb_off,
				       resp.resp_len, sb_off, is_64bit,
				       false, svc);
	if (!ret) {
		qseecom.send_resp_flag = 1;
		svc->send_resp_flag = 1;
		wake_up_interruptible(&qseecom.send_resp_wq);
	}
	mutex_unlock(&listener_access_lock);
	return ret;
}

static int qseecom_send_service_cmd(struct qseecom_dev_handle *data,
				    void __user *argp)
{
	struct qseecom_send_svc_cmd_req req;
	struct qseecom_command_scm_resp resp = {};
	u64 args[1];
	u32 tz_cmd;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	switch (req.cmd_id) {
	case QSEOS_RPMB_PROVISION_KEY_COMMAND:
		if (req.cmd_req_len < sizeof(u32) || !req.cmd_req_buf)
			return -EINVAL;
		if (copy_from_user(&args[0], req.cmd_req_buf, sizeof(u32)))
			return -EFAULT;
		tz_cmd = 1;
		ret = qseecom_scm_exec_locked(TZ_OWNER_QSEE_OS, TZ_SVC_RPMB,
					      tz_cmd, TZ_ARGS_1(TZ_PARAM_VAL),
					      args, 1, &resp);
		break;
	case QSEOS_RPMB_ERASE_COMMAND:
		ret = qseecom_scm_exec_locked(TZ_OWNER_QSEE_OS, TZ_SVC_RPMB,
					      2, TZ_ARGS_0(), NULL, 0, &resp);
		break;
	case QSEOS_RPMB_CHECK_PROV_STATUS_COMMAND:
		ret = qseecom_scm_exec_locked(TZ_OWNER_QSEE_OS, TZ_SVC_RPMB,
					      3, TZ_ARGS_0(), NULL, 0, &resp);
		if (!ret && req.resp_buf && req.resp_len >= sizeof(u32)) {
			u32 st = resp.result;

			if (copy_to_user(req.resp_buf, &st, sizeof(u32)))
				return -EFAULT;
			return 0;
		}
		break;
	default:
		return -EINVAL;
	}

	if (!ret && resp.result == QSEOS_RESULT_INCOMPLETE)
		ret = qseecom_process_incomplete_cmd(data, &resp);
	else if (!ret && resp.result != QSEOS_RESULT_SUCCESS &&
		 req.cmd_id != QSEOS_RPMB_CHECK_PROV_STATUS_COMMAND)
		ret = -EFAULT;

	return ret;
}

static int qseecom_load_external_elf(struct qseecom_dev_handle *data,
				     void __user *argp)
{
	struct qseecom_load_img_req req;
	struct qseecom_dmabuf_map fw_buf = {};
	struct qseecom_command_scm_resp resp = {};
	u64 args[3];
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	ret = qseecom_dmabuf_map(req.ifd_data_fd, false, true, &fw_buf);
	if (ret)
		return ret;
	if (req.mdt_len > fw_buf.len || req.img_len > fw_buf.len) {
		qseecom_dmabuf_unmap(&fw_buf);
		return -EINVAL;
	}

	qseecom_dmabuf_sync(&fw_buf, true);
	args[0] = req.mdt_len;
	args[1] = req.img_len;
	args[2] = fw_buf.paddr;
	ret = qseecom_scm_exec_locked(TZ_OWNER_QSEE_OS, TZ_SVC_EXTERNAL, 1,
				      TZ_ARGS_3(TZ_PARAM_VAL, TZ_PARAM_VAL,
						TZ_PARAM_VAL),
				      args, 3, &resp);
	if (!ret && resp.result == QSEOS_RESULT_INCOMPLETE)
		ret = qseecom_process_incomplete_cmd(data, &resp);
	else if (!ret && resp.result != QSEOS_RESULT_SUCCESS)
		ret = -EFAULT;

	qseecom_dmabuf_unmap(&fw_buf);
	return ret;
}

static int qseecom_unload_external_elf(struct qseecom_dev_handle *data)
{
	struct qseecom_command_scm_resp resp = {};
	int ret;

	ret = qseecom_scm_exec_locked(TZ_OWNER_QSEE_OS, TZ_SVC_EXTERNAL, 2,
				      TZ_ARGS_0(), NULL, 0, &resp);
	if (!ret && resp.result == QSEOS_RESULT_INCOMPLETE)
		ret = qseecom_process_incomplete_cmd(data, &resp);
	else if (!ret && resp.result != QSEOS_RESULT_SUCCESS)
		ret = -EFAULT;
	return ret;
}

/* ---------- Kthreads & file operations ---------- */

static int qseecom_unreg_lsnr_kthread_fn(void *unused)
{
	while (!kthread_should_stop()) {
		struct qseecom_pending_lsnr_unreg *item;
		struct qseecom_listener_entry *svc;

		wait_event_interruptible(
			qseecom.unreg_lsnr_kthread_wq,
			kthread_should_stop() ||
			atomic_read(&qseecom.unreg_lsnr_state) == 1);
		if (kthread_should_stop())
			break;

		mutex_lock(&listener_access_lock);
		while (!list_empty(&qseecom.unregister_lsnr_pending_head)) {
			item = list_first_entry(
				&qseecom.unregister_lsnr_pending_head,
				struct qseecom_pending_lsnr_unreg, list);
			if (!item->data->listener.release_called)
				break;
			svc = qseecom_find_svc(item->data->listener.id);
			if (svc && __qseecom_unregister_listener(item->data, svc) == -EBUSY)
				break;
			list_del(&item->list);
			kfree(item->data);
			kfree(item);
		}
		atomic_set(&qseecom.unreg_lsnr_state, 0);
		mutex_unlock(&listener_access_lock);
		wake_up_interruptible(&qseecom.register_lsnr_pending_wq);
	}
	return 0;
}

static int qseecom_unload_app_kthread_fn(void *unused)
{
	while (!kthread_should_stop()) {
		struct qseecom_pending_app_unload *item;

		wait_event_interruptible(
			qseecom.unload_app_kthread_wq,
			kthread_should_stop() ||
			atomic_read(&qseecom.unload_app_state) == 1);
		if (kthread_should_stop())
			break;

		mutex_lock(&unload_app_pending_lock);
		while (!list_empty(&qseecom.unload_app_pending_head)) {
			item = list_first_entry(
				&qseecom.unload_app_pending_head,
				struct qseecom_pending_app_unload, list);
			mutex_unlock(&unload_app_pending_lock);

			mutex_lock(&app_access_lock);
			qseecom_unload_app(item->data, true);
			mutex_unlock(&app_access_lock);

			mutex_lock(&unload_app_pending_lock);
			list_del(&item->list);
			kfree(item->data);
			kfree(item);
		}
		atomic_set(&qseecom.unload_app_state, 0);
		mutex_unlock(&unload_app_pending_lock);
	}
	return 0;
}

static long qseecom_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct qseecom_dev_handle *data = file->private_data;
	void __user *argp = (void __user *)arg;
	int ret = 0;

	if (data->released)
		return -EINVAL;

	atomic_inc(&data->ioctl_count);

	switch (cmd) {
	case QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ:
		ret = qseecom_get_qseos_version(argp);
		break;
	case QSEECOM_IOCTL_APP_LOADED_QUERY_REQ:
		if (data->type != QSEECOM_GENERIC &&
		    data->type != QSEECOM_CLIENT_APP) {
			ret = -EINVAL;
			break;
		}
		data->type = QSEECOM_CLIENT_APP;
		mutex_lock(&app_access_lock);
		ret = qseecom_app_load_query(data, argp);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_SET_MEM_PARAM_REQ:
		if (data->type != QSEECOM_GENERIC &&
		    data->type != QSEECOM_CLIENT_APP &&
		    data->type != QSEECOM_SECURE_SERVICE) {
			ret = -EINVAL;
			break;
		}
		mutex_lock(&app_access_lock);
		ret = qseecom_set_client_mem_param(data, argp);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_LOAD_APP_REQ:
		if (data->type != QSEECOM_GENERIC &&
		    data->type != QSEECOM_CLIENT_APP) {
			ret = -EINVAL;
			break;
		}
		data->type = QSEECOM_CLIENT_APP;
		mutex_lock(&app_access_lock);
		ret = qseecom_load_app(data, argp);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_UNLOAD_APP_REQ:
		if (data->type != QSEECOM_CLIENT_APP || !data->client.app_id) {
			ret = -EINVAL;
			break;
		}
		mutex_lock(&app_access_lock);
		ret = qseecom_unload_app(data, false);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_REGISTER_LISTENER_REQ:
		if (data->type != QSEECOM_GENERIC) {
			ret = -EINVAL;
			break;
		}
		data->type = QSEECOM_LISTENER_SERVICE;
		mutex_lock(&listener_access_lock);
		ret = qseecom_register_listener(data, argp);
		mutex_unlock(&listener_access_lock);
		break;
	case QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ:
		if (data->type != QSEECOM_LISTENER_SERVICE || !data->listener.id) {
			ret = -EINVAL;
			break;
		}
		mutex_lock(&listener_access_lock);
		ret = qseecom_unregister_listener(data);
		mutex_unlock(&listener_access_lock);
		break;
	case QSEECOM_IOCTL_RECEIVE_REQ:
		if (data->type != QSEECOM_LISTENER_SERVICE || !data->listener.id) {
			ret = -EINVAL;
			break;
		}
		ret = qseecom_receive_req(data);
		break;
	case QSEECOM_IOCTL_SEND_RESP_REQ:
		if (data->type != QSEECOM_LISTENER_SERVICE || !data->listener.id) {
			ret = -EINVAL;
			break;
		}
		ret = qseecom_send_resp(data);
		break;
	case QSEECOM_IOCTL_SEND_MODFD_RESP:
	case QSEECOM_IOCTL_SEND_MODFD_RESP_64:
		if (data->type != QSEECOM_LISTENER_SERVICE || !data->listener.id) {
			ret = -EINVAL;
			break;
		}
		ret = qseecom_send_modfd_resp(
			data, argp, cmd == QSEECOM_IOCTL_SEND_MODFD_RESP_64);
		break;
	case QSEECOM_IOCTL_SEND_CMD_REQ:
		if (data->type != QSEECOM_CLIENT_APP || !data->client.app_id) {
			ret = -EINVAL;
			break;
		}
		mutex_lock(&app_access_lock);
		ret = qseecom_send_cmd_ioctl(data, argp);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_SEND_MODFD_CMD_REQ:
	case QSEECOM_IOCTL_SEND_MODFD_CMD_64_REQ:
		if (data->type != QSEECOM_CLIENT_APP || !data->client.app_id) {
			ret = -EINVAL;
			break;
		}
		mutex_lock(&app_access_lock);
		ret = qseecom_send_modfd_cmd_ioctl(
			data, argp, cmd == QSEECOM_IOCTL_SEND_MODFD_CMD_64_REQ);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_SEND_CMD_SERVICE_REQ:
		if (data->type != QSEECOM_GENERIC &&
		    data->type != QSEECOM_SECURE_SERVICE) {
			ret = -EINVAL;
			break;
		}
		data->type = QSEECOM_SECURE_SERVICE;
		mutex_lock(&app_access_lock);
		ret = qseecom_send_service_cmd(data, argp);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_LOAD_EXTERNAL_ELF_REQ:
		if (data->type != QSEECOM_GENERIC) {
			ret = -EINVAL;
			break;
		}
		data->type = QSEECOM_UNAVAILABLE_CLIENT_APP;
		data->released = true;
		mutex_lock(&app_access_lock);
		ret = qseecom_load_external_elf(data, argp);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_UNLOAD_EXTERNAL_ELF_REQ:
		if (data->type != QSEECOM_UNAVAILABLE_CLIENT_APP) {
			ret = -EINVAL;
			break;
		}
		data->released = true;
		mutex_lock(&app_access_lock);
		ret = qseecom_unload_external_elf(data);
		mutex_unlock(&app_access_lock);
		break;
	case QSEECOM_IOCTL_PERF_ENABLE_REQ:
	case QSEECOM_IOCTL_PERF_DISABLE_REQ:
	case QSEECOM_IOCTL_SET_BUS_SCALING_REQ:
		/* HLOS does not manage CE clocks or bus scaling when no-clock-support is set */
		ret = 0;
		break;
	case QSEECOM_IOCTL_IS_ES_ACTIVATED_REQ: {
		struct qseecom_is_es_activated_req es = { .is_activated = 0 };

		ret = copy_to_user(argp, &es, sizeof(es)) ? -EFAULT : 0;
		break;
	}
	default:
		ret = -ENOTTY;
		break;
	}

	if (atomic_dec_and_test(&data->ioctl_count))
		wake_up_interruptible(&data->abort_wq);
	return ret;
}

static int qseecom_open(struct inode *inode, struct file *file)
{
	struct qseecom_dev_handle *data;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->sglistinfo = dma_alloc_coherent(qseecom.dev, PAGE_SIZE,
					      &data->sglistinfo_dma, GFP_KERNEL);
	if (!data->sglistinfo) {
		kfree(data);
		return -ENOMEM;
	}

	data->type = QSEECOM_GENERIC;
	init_waitqueue_head(&data->abort_wq);
	atomic_set(&data->ioctl_count, 0);
	file->private_data = data;
	return 0;
}

static int qseecom_release(struct inode *inode, struct file *file)
{
	struct qseecom_dev_handle *data = file->private_data;
	bool free_data = true;

	if (!data)
		return 0;

	data->abort = 1;

	if (!data->released) {
		switch (data->type) {
		case QSEECOM_LISTENER_SERVICE: {
			struct qseecom_listener_entry *svc;

			mutex_lock(&listener_access_lock);
			svc = qseecom_find_svc(data->listener.id);
			if (svc && svc->unregister_pending) {
				free_data = false;
			} else if (qseecom_unregister_listener(data) == 0) {
				svc = qseecom_find_svc(data->listener.id);
				if (svc && svc->unregister_pending)
					free_data = false;
			}
			data->listener.release_called = true;
			mutex_unlock(&listener_access_lock);
			atomic_set(&qseecom.unreg_lsnr_state, 1);
			wake_up_interruptible(&qseecom.unreg_lsnr_kthread_wq);
			break;
		}
		case QSEECOM_CLIENT_APP:
			mutex_lock(&app_access_lock);
			qseecom_unload_app(data, true);
			mutex_unlock(&app_access_lock);
			break;
		default:
			qseecom_dmabuf_unmap(&data->client.sb);
			break;
		}
	}

	if (free_data) {
		if (data->sglistinfo)
			dma_free_coherent(qseecom.dev, PAGE_SIZE,
					  data->sglistinfo,
					  data->sglistinfo_dma);
		kfree(data);
	}
	return 0;
}

static const struct file_operations qseecom_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = qseecom_ioctl,
	.open = qseecom_open,
	.release = qseecom_release,
};

static struct miscdevice qseecom_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = QSEECOM_DEV_NAME,
	.fops = &qseecom_fops,
	.mode = 0660,
};

/* ---------- Kernel-client exported API ---------- */

int qseecom_start_app(struct qseecom_handle **handle, char *app_name, u32 size)
{
	struct qseecom_dev_handle *data;
	struct qseecom_command_scm_resp resp = {};
	u32 app_id = 0, mdt_len = 0, img_len = 0, app_arch = ELFCLASS64;
	void *fw_buf = NULL;
	dma_addr_t fw_dma = 0;
	u64 args[3];
	int ret;

	if (!qseecom.dev || !handle || !app_name)
		return -EINVAL;

	*handle = kzalloc(sizeof(**handle), GFP_KERNEL);
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!*handle || !data) {
		kfree(*handle);
		kfree(data);
		return -ENOMEM;
	}

	data->client.kbuf_size = PAGE_ALIGN(size);
	data->client.kbuf_virt = dma_alloc_coherent(qseecom.dev,
						    data->client.kbuf_size,
						    &data->client.kbuf_dma,
						    GFP_KERNEL);
	if (!data->client.kbuf_virt) {
		ret = -ENOMEM;
		goto err_free;
	}
	data->type = QSEECOM_CLIENT_APP;
	data->client.app_arch = app_arch;
	qseecom_normalize_app_name(data->client.app_name, app_name);

	mutex_lock(&app_access_lock);
	ret = qseecom_lookup_app_id(app_name, &app_id);
	if (!ret && !app_id) {
		ret = qseecom_read_split_fw(app_name, &fw_buf, &fw_dma,
					    &mdt_len, &img_len, &app_arch);
		if (!ret) {
			data->client.app_arch = app_arch;
			args[0] = mdt_len;
			args[1] = img_len;
			args[2] = fw_dma;
			ret = qseecom_scm_exec_locked(
				TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 1,
				TZ_ARGS_3(TZ_PARAM_VAL, TZ_PARAM_VAL,
					  TZ_PARAM_VAL),
				args, 3, &resp);
			if (!ret)
				ret = qseecom_handle_tz_resp(data, NULL, &resp);
			dma_free_coherent(qseecom.dev, PAGE_ALIGN(img_len),
					  fw_buf, fw_dma);
			if (!ret)
				app_id = resp.data;
		}
	}
	mutex_unlock(&app_access_lock);

	if (ret || !app_id) {
		ret = ret ? ret : -ENOENT;
		goto err_dma;
	}

	if (!qseecom_find_app_entry(app_id, data->client.app_name))
		qseecom_add_app_entry(app_id, app_name, app_arch);

	data->client.app_id = app_id;
	(*handle)->dev = data;
	(*handle)->sbuf = data->client.kbuf_virt;
	(*handle)->sbuf_len = size;
	return 0;

err_dma:
	dma_free_coherent(qseecom.dev, data->client.kbuf_size,
			  data->client.kbuf_virt, data->client.kbuf_dma);
err_free:
	kfree(data);
	kfree(*handle);
	*handle = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(qseecom_start_app);

int qseecom_shutdown_app(struct qseecom_handle **handle)
{
	struct qseecom_dev_handle *data;

	if (!handle || !*handle)
		return -EINVAL;

	data = (*handle)->dev;
	if (data) {
		mutex_lock(&app_access_lock);
		qseecom_unload_app(data, false);
		mutex_unlock(&app_access_lock);
		if (data->client.kbuf_virt)
			dma_free_coherent(qseecom.dev, data->client.kbuf_size,
					  data->client.kbuf_virt,
					  data->client.kbuf_dma);
		kfree(data);
	}
	kfree(*handle);
	*handle = NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(qseecom_shutdown_app);

int qseecom_send_command(struct qseecom_handle *handle, void *send_buf,
			 u32 sbuf_len, void *resp_buf, u32 rbuf_len)
{
	struct qseecom_dev_handle *data;
	struct qseecom_command_scm_resp resp = {};
	struct qseecom_app_entry *app;
	uintptr_t req_off, rsp_off;
	u64 args[5];
	int ret;

	if (!handle || !handle->dev || !send_buf || !resp_buf)
		return -EINVAL;

	data = handle->dev;
	req_off = (u8 *)send_buf - (u8 *)data->client.kbuf_virt;
	rsp_off = (u8 *)resp_buf - (u8 *)data->client.kbuf_virt;
	if (req_off > data->client.kbuf_size ||
	    sbuf_len > data->client.kbuf_size - req_off ||
	    rsp_off > data->client.kbuf_size ||
	    rbuf_len > data->client.kbuf_size - rsp_off)
		return -EINVAL;

	mutex_lock(&app_access_lock);
	app = qseecom_find_app_entry(data->client.app_id, data->client.app_name);
	if (!app) {
		mutex_unlock(&app_access_lock);
		return -ENOENT;
	}

	args[0] = data->client.app_id;
	args[1] = data->client.kbuf_dma + req_off;
	args[2] = sbuf_len;
	args[3] = data->client.kbuf_dma + rsp_off;
	args[4] = rbuf_len;

	ret = qseecom_scm_exec_locked(
		TZ_OWNER_TZ_APPS, TZ_SVC_APP_ID_PLACEHOLDER, 1,
		TZ_ARGS_5(TZ_PARAM_VAL, TZ_PARAM_BUF_RW, TZ_PARAM_VAL,
			  TZ_PARAM_BUF_RW, TZ_PARAM_VAL),
		args, 5, &resp);
	if (!ret)
		ret = qseecom_handle_tz_resp(data, app, &resp);
	mutex_unlock(&app_access_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qseecom_send_command);

/* ---------- Platform driver ---------- */

static int qseecom_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qseecom_command_scm_resp resp = {};
	u32 whitelist_ver = 0;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "failed to set 32-bit DMA mask: %d\n", ret);
		return ret;
	}

	ret = of_reserved_mem_device_init(dev);
	if (ret && ret != -ENODEV)
		return ret;

	ret = qseecom_get_feature_version(FEATURE_ID_QSEE_VERSION,
					  &qseecom.qsee_version);
	if (ret == -EPROBE_DEFER)
		goto err_rmem;
	if (ret) {
		dev_err(dev, "failed to query QSEE version: %d\n", ret);
		goto err_rmem;
	}

	qseecom.dev = dev;
	qseecom.qseos_version = QSEOS_VERSION_14;
	qseecom.no_clock_support = of_property_read_bool(dev->of_node,
							 "qcom,no-clock-support");
	qseecom.appsbl_qseecom_support =
		of_property_read_bool(dev->of_node, "qcom,appsbl-qseecom-support");
	qseecom.commonlib64_loaded =
		of_property_read_bool(dev->of_node,
				      "qcom,commonlib64-loaded-by-uefi");
	qseecom.commonlib_loaded =
		qseecom.is_apps_region_protected || qseecom.appsbl_qseecom_support;
	of_property_read_u32(dev->of_node, "qcom,qsee-reentrancy-support",
			     &qseecom.reentrancy_support);

	INIT_LIST_HEAD(&qseecom.registered_listener_list_head);
	INIT_LIST_HEAD(&qseecom.registered_app_list_head);
	spin_lock_init(&qseecom.registered_app_list_lock);
	INIT_LIST_HEAD(&qseecom.unregister_lsnr_pending_head);
	INIT_LIST_HEAD(&qseecom.unload_app_pending_head);
	init_waitqueue_head(&qseecom.send_resp_wq);
	init_waitqueue_head(&qseecom.app_block_wq);
	init_waitqueue_head(&qseecom.register_lsnr_pending_wq);
	init_waitqueue_head(&qseecom.unreg_lsnr_kthread_wq);
	init_waitqueue_head(&qseecom.unload_app_kthread_wq);

	if (qseecom.qsee_version >= QSEE_VERSION_02 &&
	    !qseecom.is_apps_region_protected &&
	    !qseecom.appsbl_qseecom_support) {
		struct resource *res = platform_get_resource_byname(
			pdev, IORESOURCE_MEM, "secapp-region");

		if (res) {
			u64 args[2] = { res->start, resource_size(res) };

			mutex_lock(&app_access_lock);
			ret = qseecom_scm_exec_locked(
				TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 5,
				TZ_ARGS_2(TZ_PARAM_BUF_RW, TZ_PARAM_VAL),
				args, 2, &resp);
			mutex_unlock(&app_access_lock);
			if (ret || resp.result != QSEOS_RESULT_SUCCESS) {
				dev_err(dev, "secapp-region SCM failed (%d/%u)\n",
					ret, resp.result);
				ret = ret ? ret : -EINVAL;
				goto err_rmem;
			}
		}
	}

	if (!qseecom_get_feature_version(FEATURE_ID_WHITELIST, &whitelist_ver))
		qseecom.whitelist_support = (whitelist_ver >= WHITELIST_V1_0_0);

	qseecom.unreg_lsnr_kthread = kthread_run(qseecom_unreg_lsnr_kthread_fn,
						 NULL, "qseecom-unreg-lsnr");
	if (IS_ERR(qseecom.unreg_lsnr_kthread)) {
		ret = PTR_ERR(qseecom.unreg_lsnr_kthread);
		goto err_rmem;
	}

	qseecom.unload_app_kthread = kthread_run(qseecom_unload_app_kthread_fn,
						 NULL, "qseecom-unload-ta");
	if (IS_ERR(qseecom.unload_app_kthread)) {
		ret = PTR_ERR(qseecom.unload_app_kthread);
		goto err_stop_unreg;
	}

	ret = misc_register(&qseecom_miscdev);
	if (ret) {
		dev_err(dev, "misc_register failed: %d\n", ret);
		goto err_stop_unload;
	}

	dev_info(dev, "initialized (qsee_version=0x%x whitelist=%d reentrancy=%u)\n",
		 qseecom.qsee_version, qseecom.whitelist_support,
		 qseecom.reentrancy_support);
	return 0;

err_stop_unload:
	kthread_stop(qseecom.unload_app_kthread);
err_stop_unreg:
	kthread_stop(qseecom.unreg_lsnr_kthread);
err_rmem:
	of_reserved_mem_device_release(dev);
	return ret;
}

static void qseecom_remove(struct platform_device *pdev)
{
	misc_deregister(&qseecom_miscdev);
	if (qseecom.unload_app_kthread)
		kthread_stop(qseecom.unload_app_kthread);
	if (qseecom.unreg_lsnr_kthread)
		kthread_stop(qseecom.unreg_lsnr_kthread);
	of_reserved_mem_device_release(&pdev->dev);
	qseecom.dev = NULL;
}

static const struct of_device_id qseecom_of_match[] = {
	{ .compatible = "qcom,qseecom" },
	{ }
};
MODULE_DEVICE_TABLE(of, qseecom_of_match);

static struct platform_driver qseecom_plat_driver = {
	.probe = qseecom_probe,
	.remove = qseecom_remove,
	.driver = {
		.name = "qseecom",
		.of_match_table = qseecom_of_match,
	},
};
module_platform_driver(qseecom_plat_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm TrustZone QSEECOM (/dev/qseecom) Driver");
