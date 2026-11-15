/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Qualcomm TrustZone QSEECOM protocol definitions.
 * Derived from stock OnePlus MSM8998 QSEECOM interface headers.
 */
#ifndef __QSEECOMI_H_
#define __QSEECOMI_H_

#include <linux/types.h>
#include <uapi/linux/qseecom.h>

#define TZ_OWNER_SIP			2
#define TZ_OWNER_TZ_APPS		48
#define TZ_OWNER_QSEE_OS		50

#define TZ_SVC_APP_ID_PLACEHOLDER	0
#define TZ_SVC_APP_MGR			1
#define TZ_SVC_LISTENER			2
#define TZ_SVC_EXTERNAL			3
#define TZ_SVC_RPMB			4
#define TZ_SVC_KEYSTORE			5
#define TZ_SVC_ES			16
#define TZ_SVC_MDTP			18
#define TZ_SVC_INFO			6

#define TZ_PARAM_VAL			0x0
#define TZ_PARAM_BUF_RO			0x1
#define TZ_PARAM_BUF_RW			0x2

#define TZ_ARGS_0() 0
#define TZ_ARGS_1(a) (1 | ((a) << 4))
#define TZ_ARGS_2(a, b) (2 | ((a) << 4) | ((b) << 6))
#define TZ_ARGS_3(a, b, c) (3 | ((a) << 4) | ((b) << 6) | ((c) << 8))
#define TZ_ARGS_4(a, b, c, d) \
	(4 | ((a) << 4) | ((b) << 6) | ((c) << 8) | ((d) << 10))
#define TZ_ARGS_5(a, b, c, d, e) \
	(5 | ((a) << 4) | ((b) << 6) | ((c) << 8) | ((d) << 10) | ((e) << 12))
#define TZ_ARGS_7(a, b, c, d, e, f, g) \
	(7 | ((a) << 4) | ((b) << 6) | ((c) << 8) | ((d) << 10) | \
	 ((e) << 12) | ((f) << 14) | ((g) << 16))

/* Command IDs inside SCM_SVC_TZSCHEDULER */
enum qseecom_qceos_cmd_id {
	QSEOS_APP_START_COMMAND			= 0x01,
	QSEOS_APP_SHUTDOWN_COMMAND		= 0x02,
	QSEOS_APP_LOOKUP_COMMAND		= 0x03,
	QSEOS_REGISTER_LISTENER			= 0x04,
	QSEOS_DEREGISTER_LISTENER		= 0x05,
	QSEOS_CLIENT_SEND_DATA_COMMAND		= 0x06,
	QSEOS_LISTENER_DATA_RSP_COMMAND		= 0x07,
	QSEOS_LOAD_EXTERNAL_ELF_COMMAND		= 0x08,
	QSEOS_UNLOAD_EXTERNAL_ELF_COMMAND	= 0x09,
	QSEOS_GET_APP_STATE_COMMAND		= 0x0A,
	QSEOS_LOAD_SERV_IMAGE_COMMAND		= 0x0B,
	QSEOS_UNLOAD_SERV_IMAGE_COMMAND		= 0x0C,
	QSEOS_APP_REGION_NOTIFICATION		= 0x0D,
	QSEOS_REGISTER_LOG_BUF_COMMAND		= 0x0E,
	QSEOS_RPMB_PROVISION_KEY_COMMAND	= 0x0F,
	QSEOS_RPMB_ERASE_COMMAND		= 0x10,
	QSEOS_GENERATE_KEY			= 0x11,
	QSEOS_DELETE_KEY			= 0x12,
	QSEOS_SET_KEY				= 0x13,
	QSEOS_UPDATE_KEY_USERINFO		= 0x14,
	QSEOS_TEE_OPEN_SESSION			= 0x15,
	QSEOS_TEE_INVOKE_COMMAND		= 0x16,
	QSEOS_TEE_CLOSE_SESSION			= 0x17,
	QSEOS_TEE_REQUEST_CANCELLATION		= 0x18,
	QSEOS_CONTINUE_BLOCKED_REQ_COMMAND	= 0x1A,
	QSEOS_RPMB_CHECK_PROV_STATUS_COMMAND	= 0x1B,
	QSEOS_CLIENT_SEND_DATA_COMMAND_WHITELIST = 0x1C,
	QSEOS_TEE_OPEN_SESSION_WHITELIST	= 0x1D,
	QSEOS_TEE_INVOKE_COMMAND_WHITELIST	= 0x1E,
	QSEOS_LISTENER_DATA_RSP_COMMAND_WHITELIST = 0x1F,
};

enum qseecom_qceos_cmd_status {
	QSEOS_RESULT_SUCCESS			= 0,
	QSEOS_RESULT_INCOMPLETE			= 1,
	QSEOS_RESULT_BLOCKED_ON_LISTENER	= 2,
	QSEOS_RESULT_FAILURE			= 0xFFFFFFFF,
};

enum qseecom_command_scm_resp_type {
	QSEOS_APP_ID				= 0xEE01,
	QSEOS_LISTENER_ID			= 0xEE02,
};

struct qseecom_command_scm_resp {
	u32 result;
	u32 resp_type;
	u32 data;
};

#endif /* __QSEECOMI_H_ */
