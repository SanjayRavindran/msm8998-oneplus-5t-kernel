// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm MSM8998 Network-on-Chip (NoC) interconnect driver
 */

#include <linux/device.h>
#include <linux/interconnect-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/interconnect/qcom,msm8998.h>

#include "icc-rpm.h"

enum {
	MSM8998_MASTER_PCIE_0 = 1,
	MSM8998_MASTER_USB3,
	MSM8998_MASTER_UFS,
	MSM8998_MASTER_BLSP_2,
	MSM8998_SLAVE_A1NOC_SNOC,

	MSM8998_MASTER_CNOC_A2NOC,
	MSM8998_MASTER_IPA,
	MSM8998_MASTER_SDCC_2,
	MSM8998_MASTER_SDCC_4,
	MSM8998_MASTER_TSIF,
	MSM8998_MASTER_BLSP_1,
	MSM8998_MASTER_CRVIRT_A2NOC,
	MSM8998_MASTER_CRYPTO_CORE0,
	MSM8998_SLAVE_CRVIRT_A2NOC,
	MSM8998_SLAVE_A2NOC_SNOC,

	MSM8998_MASTER_GNOC_BIMC,
	MSM8998_MASTER_OXILI,
	MSM8998_MASTER_MNOC_BIMC,
	MSM8998_MASTER_SNOC_BIMC,
	MSM8998_SLAVE_EBI,
	MSM8998_SLAVE_HMSS_L3,
	MSM8998_SLAVE_BIMC_SNOC_0,
	MSM8998_SLAVE_BIMC_SNOC_1,

	MSM8998_MASTER_SNOC_CNOC,
	MSM8998_MASTER_QDSS_DAP,
	MSM8998_SLAVE_CNOC_A2NOC,
	MSM8998_SLAVE_SSC_CFG,
	MSM8998_SLAVE_MPM,
	MSM8998_SLAVE_PMIC_ARB,
	MSM8998_SLAVE_TLMM_NORTH,
	MSM8998_SLAVE_PIMEM_CFG,
	MSM8998_SLAVE_IMEM_CFG,
	MSM8998_SLAVE_MESSAGE_RAM,
	MSM8998_SLAVE_SKL,
	MSM8998_SLAVE_BIMC_CFG,
	MSM8998_SLAVE_PRNG,
	MSM8998_SLAVE_A2NOC_CFG,
	MSM8998_SLAVE_IPA_CFG,
	MSM8998_SLAVE_TCSR,
	MSM8998_SLAVE_SNOC_CFG,
	MSM8998_SLAVE_CLK_CTL,
	MSM8998_SLAVE_GLM,
	MSM8998_SLAVE_SPDM,
	MSM8998_SLAVE_GPUSS_CFG,
	MSM8998_SLAVE_CNOC_MNOC_CFG,
	MSM8998_SLAVE_QM_CFG,
	MSM8998_SLAVE_MSS_CFG,
	MSM8998_SLAVE_UFS_CFG,
	MSM8998_SLAVE_TLMM_WEST,
	MSM8998_SLAVE_A1NOC_CFG,
	MSM8998_SLAVE_AHB2PHY,
	MSM8998_SLAVE_BLSP_2,
	MSM8998_SLAVE_PDM,
	MSM8998_SLAVE_USB3_0,
	MSM8998_SLAVE_A1NOC_SMMU_CFG,
	MSM8998_SLAVE_BLSP_1,
	MSM8998_SLAVE_SDCC_2,
	MSM8998_SLAVE_SDCC_4,
	MSM8998_SLAVE_TSIF,
	MSM8998_SLAVE_QDSS_CFG,
	MSM8998_SLAVE_TLMM_EAST,
	MSM8998_SLAVE_CNOC_MNOC_MMSS_CFG,
	MSM8998_SLAVE_SRVC_CNOC,

	MSM8998_MASTER_APPS_PROC,
	MSM8998_SLAVE_GNOC_BIMC,

	MSM8998_MASTER_CNOC_MNOC_MMSS_CFG,
	MSM8998_MASTER_CNOC_MNOC_CFG,
	MSM8998_MASTER_CPP,
	MSM8998_MASTER_JPEG,
	MSM8998_MASTER_MDP_P0,
	MSM8998_MASTER_MDP_P1,
	MSM8998_MASTER_ROTATOR,
	MSM8998_MASTER_VIDEO_P0,
	MSM8998_MASTER_VIDEO_P1,
	MSM8998_MASTER_VFE,
	MSM8998_MASTER_VENUS_VMEM,
	MSM8998_SLAVE_CAMERA_CFG,
	MSM8998_SLAVE_CAMERA_THROTTLE_CFG,
	MSM8998_SLAVE_MISC_CFG,
	MSM8998_SLAVE_VENUS_THROTTLE_CFG,
	MSM8998_SLAVE_VENUS_CFG,
	MSM8998_SLAVE_VMEM_CFG,
	MSM8998_SLAVE_MMSS_CLK_XPU_CFG,
	MSM8998_SLAVE_MMSS_CLK_CFG,
	MSM8998_SLAVE_DISPLAY_CFG,
	MSM8998_SLAVE_DISPLAY_THROTTLE_CFG,
	MSM8998_SLAVE_SMMU_CFG,
	MSM8998_SLAVE_MNOC_BIMC,
	MSM8998_SLAVE_VMEM,
	MSM8998_SLAVE_SRVC_MNOC,

	MSM8998_MASTER_HMSS,
	MSM8998_MASTER_QDSS_BAM,
	MSM8998_MASTER_SNOC_CFG,
	MSM8998_MASTER_BIMC_SNOC_0,
	MSM8998_MASTER_BIMC_SNOC_1,
	MSM8998_MASTER_A1NOC_SNOC,
	MSM8998_MASTER_A2NOC_SNOC,
	MSM8998_MASTER_QDSS_ETR,
	MSM8998_SLAVE_HMSS,
	MSM8998_SLAVE_LPASS,
	MSM8998_SLAVE_WLAN,
	MSM8998_SLAVE_SNOC_BIMC,
	MSM8998_SLAVE_SNOC_CNOC,
	MSM8998_SLAVE_IMEM,
	MSM8998_SLAVE_PIMEM,
	MSM8998_SLAVE_QDSS_STM,
	MSM8998_SLAVE_PCIE_0,
	MSM8998_SLAVE_SRVC_SNOC,
};

#define DEFINE_QNODE(_name, _id, _bw, _ch, _mas_rpm, _slv_rpm,		\
		     _ap, _qos_mode, _prio, _qport, _ib_coeff, _links)	\
	static struct qcom_icc_node _name = {					\
		.name = #_name,						\
		.id = _id,						\
		.buswidth = _bw,					\
		.channels = _ch,					\
		.mas_rpm_id = _mas_rpm,					\
		.slv_rpm_id = _slv_rpm,					\
		.ib_coeff = _ib_coeff,					\
		.qos.ap_owned = _ap,					\
		.qos.qos_mode = _qos_mode,				\
		.qos.areq_prio = _prio,					\
		.qos.prio_level = _prio,				\
		.qos.qos_port = _qport,					\
		.num_links = ARRAY_SIZE(_links),			\
		.links = _links,					\
	}

#define DEFINE_QNODE_ENDP(_name, _id, _bw, _ch, _slv_rpm, _ap)		\
	static struct qcom_icc_node _name = {					\
		.name = #_name,						\
		.id = _id,						\
		.buswidth = _bw,					\
		.channels = _ch,					\
		.mas_rpm_id = -1,					\
		.slv_rpm_id = _slv_rpm,					\
		.qos.ap_owned = _ap,					\
		.qos.qos_mode = NOC_QOS_MODE_INVALID,			\
		.qos.qos_port = -1,					\
	}

static const char * const a1noc_intf_clocks[] = {
	"ufs_axi",
	"aggre1_ufs_axi",
	"aggre1_usb3_axi",
};

static const char * const a2noc_intf_clocks[] = {
	"ipa",
};

static const char * const mnoc_intf_clocks[] = {
	"iface",
};

/* Links */
static const u16 mas_a1noc_common_links[] = { MSM8998_SLAVE_A1NOC_SNOC };
static const u16 slv_a1noc_snoc_links[] = { MSM8998_MASTER_A1NOC_SNOC };

static const u16 mas_a2noc_common_links[] = { MSM8998_SLAVE_A2NOC_SNOC };
static const u16 mas_crypto_c0_links[] = { MSM8998_SLAVE_CRVIRT_A2NOC };
static const u16 slv_cr_virt_a2noc_links[] = { MSM8998_MASTER_CRVIRT_A2NOC };
static const u16 slv_a2noc_snoc_links[] = { MSM8998_MASTER_A2NOC_SNOC };

static const u16 mas_gnoc_bimc_links[] = {
	MSM8998_SLAVE_EBI,
	MSM8998_SLAVE_BIMC_SNOC_0,
};
static const u16 mas_bimc_all_links[] = {
	MSM8998_SLAVE_BIMC_SNOC_1,
	MSM8998_SLAVE_HMSS_L3,
	MSM8998_SLAVE_EBI,
	MSM8998_SLAVE_BIMC_SNOC_0,
};
static const u16 mas_snoc_bimc_links[] = {
	MSM8998_SLAVE_HMSS_L3,
	MSM8998_SLAVE_EBI,
};
static const u16 slv_bimc_snoc_0_links[] = { MSM8998_MASTER_BIMC_SNOC_0 };
static const u16 slv_bimc_snoc_1_links[] = { MSM8998_MASTER_BIMC_SNOC_1 };

static const u16 mas_snoc_cnoc_links[] = {
	MSM8998_SLAVE_SKL,
	MSM8998_SLAVE_BLSP_2,
	MSM8998_SLAVE_MESSAGE_RAM,
	MSM8998_SLAVE_TLMM_WEST,
	MSM8998_SLAVE_TSIF,
	MSM8998_SLAVE_MPM,
	MSM8998_SLAVE_BIMC_CFG,
	MSM8998_SLAVE_TLMM_EAST,
	MSM8998_SLAVE_SPDM,
	MSM8998_SLAVE_PIMEM_CFG,
	MSM8998_SLAVE_A1NOC_SMMU_CFG,
	MSM8998_SLAVE_BLSP_1,
	MSM8998_SLAVE_CLK_CTL,
	MSM8998_SLAVE_PRNG,
	MSM8998_SLAVE_USB3_0,
	MSM8998_SLAVE_QDSS_CFG,
	MSM8998_SLAVE_QM_CFG,
	MSM8998_SLAVE_A2NOC_CFG,
	MSM8998_SLAVE_PMIC_ARB,
	MSM8998_SLAVE_UFS_CFG,
	MSM8998_SLAVE_SRVC_CNOC,
	MSM8998_SLAVE_AHB2PHY,
	MSM8998_SLAVE_IPA_CFG,
	MSM8998_SLAVE_GLM,
	MSM8998_SLAVE_SNOC_CFG,
	MSM8998_SLAVE_SSC_CFG,
	MSM8998_SLAVE_SDCC_2,
	MSM8998_SLAVE_SDCC_4,
	MSM8998_SLAVE_PDM,
	MSM8998_SLAVE_CNOC_MNOC_MMSS_CFG,
	MSM8998_SLAVE_CNOC_MNOC_CFG,
	MSM8998_SLAVE_MSS_CFG,
	MSM8998_SLAVE_IMEM_CFG,
	MSM8998_SLAVE_A1NOC_CFG,
	MSM8998_SLAVE_GPUSS_CFG,
	MSM8998_SLAVE_TCSR,
	MSM8998_SLAVE_TLMM_NORTH,
};

static const u16 mas_qdss_dap_links[] = {
	MSM8998_SLAVE_SKL,
	MSM8998_SLAVE_BLSP_2,
	MSM8998_SLAVE_MESSAGE_RAM,
	MSM8998_SLAVE_TLMM_WEST,
	MSM8998_SLAVE_TSIF,
	MSM8998_SLAVE_MPM,
	MSM8998_SLAVE_BIMC_CFG,
	MSM8998_SLAVE_TLMM_EAST,
	MSM8998_SLAVE_SPDM,
	MSM8998_SLAVE_PIMEM_CFG,
	MSM8998_SLAVE_A1NOC_SMMU_CFG,
	MSM8998_SLAVE_BLSP_1,
	MSM8998_SLAVE_CLK_CTL,
	MSM8998_SLAVE_PRNG,
	MSM8998_SLAVE_USB3_0,
	MSM8998_SLAVE_QDSS_CFG,
	MSM8998_SLAVE_QM_CFG,
	MSM8998_SLAVE_A2NOC_CFG,
	MSM8998_SLAVE_PMIC_ARB,
	MSM8998_SLAVE_UFS_CFG,
	MSM8998_SLAVE_SRVC_CNOC,
	MSM8998_SLAVE_AHB2PHY,
	MSM8998_SLAVE_IPA_CFG,
	MSM8998_SLAVE_GLM,
	MSM8998_SLAVE_SNOC_CFG,
	MSM8998_SLAVE_SSC_CFG,
	MSM8998_SLAVE_SDCC_2,
	MSM8998_SLAVE_SDCC_4,
	MSM8998_SLAVE_PDM,
	MSM8998_SLAVE_CNOC_MNOC_MMSS_CFG,
	MSM8998_SLAVE_CNOC_MNOC_CFG,
	MSM8998_SLAVE_MSS_CFG,
	MSM8998_SLAVE_IMEM_CFG,
	MSM8998_SLAVE_A1NOC_CFG,
	MSM8998_SLAVE_GPUSS_CFG,
	MSM8998_SLAVE_TCSR,
	MSM8998_SLAVE_TLMM_NORTH,
	MSM8998_SLAVE_CNOC_A2NOC,
};

static const u16 slv_cnoc_a2noc_links[] = { MSM8998_MASTER_CNOC_A2NOC };
static const u16 slv_cnoc_mnoc_cfg_links[] = { MSM8998_MASTER_CNOC_MNOC_CFG };
static const u16 slv_cnoc_mnoc_mmss_cfg_links[] = { MSM8998_MASTER_CNOC_MNOC_MMSS_CFG };

static const u16 mas_apps_proc_links[] = { MSM8998_SLAVE_GNOC_BIMC };
static const u16 slv_gnoc_bimc_links[] = { MSM8998_MASTER_GNOC_BIMC };

static const u16 mas_mnoc_bimc_links[] = { MSM8998_SLAVE_MNOC_BIMC };
static const u16 mas_cnoc_mnoc_cfg_links[] = { MSM8998_SLAVE_SRVC_MNOC };
static const u16 mas_venus_vmem_links[] = { MSM8998_SLAVE_VMEM };
static const u16 slv_mnoc_bimc_links[] = { MSM8998_MASTER_MNOC_BIMC };
static const u16 mas_cnoc_mnoc_mmss_cfg_links[] = {
	MSM8998_SLAVE_CAMERA_THROTTLE_CFG,
	MSM8998_SLAVE_VENUS_CFG,
	MSM8998_SLAVE_MISC_CFG,
	MSM8998_SLAVE_CAMERA_CFG,
	MSM8998_SLAVE_DISPLAY_THROTTLE_CFG,
	MSM8998_SLAVE_VENUS_THROTTLE_CFG,
	MSM8998_SLAVE_DISPLAY_CFG,
	MSM8998_SLAVE_MMSS_CLK_CFG,
	MSM8998_SLAVE_VMEM_CFG,
	MSM8998_SLAVE_MMSS_CLK_XPU_CFG,
	MSM8998_SLAVE_SMMU_CFG,
};

static const u16 mas_hmss_links[] = {
	MSM8998_SLAVE_PIMEM,
	MSM8998_SLAVE_IMEM,
	MSM8998_SLAVE_SNOC_BIMC,
};
static const u16 mas_qdss_snoc_links[] = {
	MSM8998_SLAVE_IMEM,
	MSM8998_SLAVE_PIMEM,
	MSM8998_SLAVE_SNOC_CNOC,
	MSM8998_SLAVE_SNOC_BIMC,
};
static const u16 mas_snoc_cfg_links[] = { MSM8998_SLAVE_SRVC_SNOC };
static const u16 mas_bimc_snoc_0_links[] = {
	MSM8998_SLAVE_PIMEM,
	MSM8998_SLAVE_LPASS,
	MSM8998_SLAVE_HMSS,
	MSM8998_SLAVE_WLAN,
	MSM8998_SLAVE_SNOC_CNOC,
	MSM8998_SLAVE_IMEM,
	MSM8998_SLAVE_QDSS_STM,
};
static const u16 mas_bimc_snoc_1_links[] = { MSM8998_SLAVE_PCIE_0 };
static const u16 mas_a1noc_snoc_links[] = {
	MSM8998_SLAVE_PIMEM,
	MSM8998_SLAVE_PCIE_0,
	MSM8998_SLAVE_LPASS,
	MSM8998_SLAVE_HMSS,
	MSM8998_SLAVE_SNOC_BIMC,
	MSM8998_SLAVE_SNOC_CNOC,
	MSM8998_SLAVE_IMEM,
	MSM8998_SLAVE_QDSS_STM,
};
static const u16 mas_a2noc_snoc_links[] = {
	MSM8998_SLAVE_PIMEM,
	MSM8998_SLAVE_PCIE_0,
	MSM8998_SLAVE_LPASS,
	MSM8998_SLAVE_HMSS,
	MSM8998_SLAVE_SNOC_BIMC,
	MSM8998_SLAVE_WLAN,
	MSM8998_SLAVE_SNOC_CNOC,
	MSM8998_SLAVE_IMEM,
	MSM8998_SLAVE_QDSS_STM,
};
static const u16 slv_snoc_bimc_links[] = { MSM8998_MASTER_SNOC_BIMC };
static const u16 slv_snoc_cnoc_links[] = { MSM8998_MASTER_SNOC_CNOC };

/* A1NOC nodes */
DEFINE_QNODE(mas_pcie_0, MSM8998_MASTER_PCIE_0, 16, 0, 65, -1, true, NOC_QOS_MODE_FIXED, 1, 1, 0, mas_a1noc_common_links);
DEFINE_QNODE(mas_usb3, MSM8998_MASTER_USB3, 16, 0, 32, -1, true, NOC_QOS_MODE_FIXED, 1, 2, 0, mas_a1noc_common_links);
DEFINE_QNODE(mas_ufs, MSM8998_MASTER_UFS, 16, 0, 68, -1, true, NOC_QOS_MODE_FIXED, 1, 0, 0, mas_a1noc_common_links);
DEFINE_QNODE(mas_blsp_2, MSM8998_MASTER_BLSP_2, 16, 0, 39, -1, false, NOC_QOS_MODE_FIXED, 0, 4, 0, mas_a1noc_common_links);
DEFINE_QNODE(slv_a1noc_snoc, MSM8998_SLAVE_A1NOC_SNOC, 16, 0, -1, 142, false, NOC_QOS_MODE_INVALID, 0, -1, 0, slv_a1noc_snoc_links);

/* A2NOC nodes */
DEFINE_QNODE(mas_cnoc_a2noc, MSM8998_MASTER_CNOC_A2NOC, 8, 0, 146, -1, true, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_a2noc_common_links);
DEFINE_QNODE(mas_ipa, MSM8998_MASTER_IPA, 8, 0, 59, -1, true, NOC_QOS_MODE_FIXED, 1, 1, 0, mas_a2noc_common_links);
DEFINE_QNODE(mas_sdcc_2, MSM8998_MASTER_SDCC_2, 8, 0, 35, -1, false, NOC_QOS_MODE_FIXED, 0, 6, 0, mas_a2noc_common_links);
DEFINE_QNODE(mas_sdcc_4, MSM8998_MASTER_SDCC_4, 8, 0, 36, -1, false, NOC_QOS_MODE_FIXED, 0, 7, 0, mas_a2noc_common_links);
DEFINE_QNODE(mas_tsif, MSM8998_MASTER_TSIF, 4, 0, 37, -1, true, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_a2noc_common_links);
DEFINE_QNODE(mas_blsp_1, MSM8998_MASTER_BLSP_1, 16, 0, 41, -1, false, NOC_QOS_MODE_FIXED, 0, 8, 0, mas_a2noc_common_links);
DEFINE_QNODE(mas_cr_virt_a2noc, MSM8998_MASTER_CRVIRT_A2NOC, 8, 0, 145, -1, false, NOC_QOS_MODE_FIXED, 0, 9, 0, mas_a2noc_common_links);
DEFINE_QNODE(mas_crypto_c0, MSM8998_MASTER_CRYPTO_CORE0, 650, 0, 23, -1, false, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_crypto_c0_links);
DEFINE_QNODE(slv_cr_virt_a2noc, MSM8998_SLAVE_CRVIRT_A2NOC, 8, 0, -1, 207, true, NOC_QOS_MODE_INVALID, 0, -1, 0, slv_cr_virt_a2noc_links);
DEFINE_QNODE(slv_a2noc_snoc, MSM8998_SLAVE_A2NOC_SNOC, 16, 0, -1, 143, false, NOC_QOS_MODE_INVALID, 0, -1, 0, slv_a2noc_snoc_links);

/* BIMC nodes */
DEFINE_QNODE(mas_gnoc_bimc, MSM8998_MASTER_GNOC_BIMC, 8, 2, 144, -1, true, NOC_QOS_MODE_FIXED, 0, 0, 0, mas_gnoc_bimc_links);
DEFINE_QNODE(mas_oxili, MSM8998_MASTER_OXILI, 8, 2, 6, -1, true, NOC_QOS_MODE_BYPASS, 0, 1, 0, mas_bimc_all_links);
DEFINE_QNODE(mas_mnoc_bimc, MSM8998_MASTER_MNOC_BIMC, 8, 2, 2, -1, true, NOC_QOS_MODE_BYPASS, 0, 2, 0, mas_bimc_all_links);
DEFINE_QNODE(mas_snoc_bimc, MSM8998_MASTER_SNOC_BIMC, 8, 2, 3, -1, false, NOC_QOS_MODE_BYPASS, 0, 3, 0, mas_snoc_bimc_links);
DEFINE_QNODE_ENDP(slv_ebi, MSM8998_SLAVE_EBI, 8, 2, 0, false);
DEFINE_QNODE_ENDP(slv_hmss_l3, MSM8998_SLAVE_HMSS_L3, 8, 0, 160, false);
DEFINE_QNODE(slv_bimc_snoc_0, MSM8998_SLAVE_BIMC_SNOC_0, 8, 0, -1, 2, false, NOC_QOS_MODE_INVALID, 0, -1, 0, slv_bimc_snoc_0_links);
DEFINE_QNODE(slv_bimc_snoc_1, MSM8998_SLAVE_BIMC_SNOC_1, 8, 0, -1, 138, true, NOC_QOS_MODE_INVALID, 0, -1, 0, slv_bimc_snoc_1_links);

/* CNOC nodes */
DEFINE_QNODE(mas_snoc_cnoc, MSM8998_MASTER_SNOC_CNOC, 8, 0, 52, -1, true, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_snoc_cnoc_links);
DEFINE_QNODE(mas_qdss_dap, MSM8998_MASTER_QDSS_DAP, 8, 0, 49, -1, true, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_qdss_dap_links);
DEFINE_QNODE(slv_cnoc_a2noc, MSM8998_SLAVE_CNOC_A2NOC, 4, 0, -1, 208, true, NOC_QOS_MODE_INVALID, 0, -1, 0, slv_cnoc_a2noc_links);
DEFINE_QNODE_ENDP(slv_ssc_cfg, MSM8998_SLAVE_SSC_CFG, 4, 0, 177, true);
DEFINE_QNODE_ENDP(slv_mpm, MSM8998_SLAVE_MPM, 4, 0, 62, true);
DEFINE_QNODE_ENDP(slv_pmic_arb, MSM8998_SLAVE_PMIC_ARB, 4, 0, 59, true);
DEFINE_QNODE_ENDP(slv_tlmm_north, MSM8998_SLAVE_TLMM_NORTH, 4, 0, 214, true);
DEFINE_QNODE_ENDP(slv_pimem_cfg, MSM8998_SLAVE_PIMEM_CFG, 4, 0, 167, true);
DEFINE_QNODE_ENDP(slv_imem_cfg, MSM8998_SLAVE_IMEM_CFG, 4, 0, 54, true);
DEFINE_QNODE_ENDP(slv_message_ram, MSM8998_SLAVE_MESSAGE_RAM, 4, 0, 55, true);
DEFINE_QNODE_ENDP(slv_skl, MSM8998_SLAVE_SKL, 4, 0, 196, true);
DEFINE_QNODE_ENDP(slv_bimc_cfg, MSM8998_SLAVE_BIMC_CFG, 4, 0, 56, true);
DEFINE_QNODE_ENDP(slv_prng, MSM8998_SLAVE_PRNG, 4, 0, 44, true);
DEFINE_QNODE_ENDP(slv_a2noc_cfg, MSM8998_SLAVE_A2NOC_CFG, 4, 0, 150, true);
DEFINE_QNODE_ENDP(slv_ipa_cfg, MSM8998_SLAVE_IPA_CFG, 4, 0, 183, true);
DEFINE_QNODE_ENDP(slv_tcsr, MSM8998_SLAVE_TCSR, 4, 0, 50, true);
DEFINE_QNODE_ENDP(slv_snoc_cfg, MSM8998_SLAVE_SNOC_CFG, 4, 0, 70, true);
DEFINE_QNODE_ENDP(slv_clk_ctl, MSM8998_SLAVE_CLK_CTL, 4, 0, 47, true);
DEFINE_QNODE_ENDP(slv_glm, MSM8998_SLAVE_GLM, 4, 0, 209, true);
DEFINE_QNODE_ENDP(slv_spdm, MSM8998_SLAVE_SPDM, 4, 0, 60, true);
DEFINE_QNODE_ENDP(slv_gpuss_cfg, MSM8998_SLAVE_GPUSS_CFG, 4, 0, 11, true);
DEFINE_QNODE(slv_cnoc_mnoc_cfg, MSM8998_SLAVE_CNOC_MNOC_CFG, 4, 0, -1, 66, true, NOC_QOS_MODE_INVALID, 0, -1, 0, slv_cnoc_mnoc_cfg_links);
DEFINE_QNODE_ENDP(slv_qm_cfg, MSM8998_SLAVE_QM_CFG, 4, 0, 212, true);
DEFINE_QNODE_ENDP(slv_mss_cfg, MSM8998_SLAVE_MSS_CFG, 4, 0, 48, true);
DEFINE_QNODE_ENDP(slv_ufs_cfg, MSM8998_SLAVE_UFS_CFG, 4, 0, 92, true);
DEFINE_QNODE_ENDP(slv_tlmm_west, MSM8998_SLAVE_TLMM_WEST, 4, 0, 215, true);
DEFINE_QNODE_ENDP(slv_a1noc_cfg, MSM8998_SLAVE_A1NOC_CFG, 4, 0, 147, true);
DEFINE_QNODE_ENDP(slv_ahb2phy, MSM8998_SLAVE_AHB2PHY, 4, 0, 163, true);
DEFINE_QNODE_ENDP(slv_blsp_2, MSM8998_SLAVE_BLSP_2, 4, 0, 37, true);
DEFINE_QNODE_ENDP(slv_pdm, MSM8998_SLAVE_PDM, 4, 0, 41, true);
DEFINE_QNODE_ENDP(slv_usb3_0, MSM8998_SLAVE_USB3_0, 4, 0, 22, true);
DEFINE_QNODE_ENDP(slv_a1noc_smmu_cfg, MSM8998_SLAVE_A1NOC_SMMU_CFG, 8, 0, 149, true);
DEFINE_QNODE_ENDP(slv_blsp_1, MSM8998_SLAVE_BLSP_1, 4, 0, 39, true);
DEFINE_QNODE_ENDP(slv_sdcc_2, MSM8998_SLAVE_SDCC_2, 4, 0, 33, true);
DEFINE_QNODE_ENDP(slv_sdcc_4, MSM8998_SLAVE_SDCC_4, 4, 0, 34, true);
DEFINE_QNODE_ENDP(slv_tsif, MSM8998_SLAVE_TSIF, 4, 0, 35, true);
DEFINE_QNODE_ENDP(slv_qdss_cfg, MSM8998_SLAVE_QDSS_CFG, 4, 0, 63, true);
DEFINE_QNODE_ENDP(slv_tlmm_east, MSM8998_SLAVE_TLMM_EAST, 4, 0, 213, true);
DEFINE_QNODE(slv_cnoc_mnoc_mmss_cfg, MSM8998_SLAVE_CNOC_MNOC_MMSS_CFG, 8, 0, -1, 58, true, NOC_QOS_MODE_INVALID, 0, -1, 0, slv_cnoc_mnoc_mmss_cfg_links);
DEFINE_QNODE_ENDP(slv_srvc_cnoc, MSM8998_SLAVE_SRVC_CNOC, 4, 0, 76, true);

/* GNOC nodes */
DEFINE_QNODE(mas_apps_proc, MSM8998_MASTER_APPS_PROC, 32, 0, 0, -1, true, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_apps_proc_links);
DEFINE_QNODE(slv_gnoc_bimc, MSM8998_SLAVE_GNOC_BIMC, 32, 0, -1, 210, true, NOC_QOS_MODE_INVALID, 0, -1, 0, slv_gnoc_bimc_links);

/* MNOC nodes */
DEFINE_QNODE(mas_cnoc_mnoc_mmss_cfg, MSM8998_MASTER_CNOC_MNOC_MMSS_CFG, 8, 0, 4, -1, true, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_cnoc_mnoc_mmss_cfg_links);
DEFINE_QNODE(mas_cnoc_mnoc_cfg, MSM8998_MASTER_CNOC_MNOC_CFG, 8, 0, 5, -1, true, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_cnoc_mnoc_cfg_links);
DEFINE_QNODE(mas_cpp, MSM8998_MASTER_CPP, 32, 0, 115, -1, true, NOC_QOS_MODE_BYPASS, 0, 5, 0, mas_mnoc_bimc_links);
DEFINE_QNODE(mas_jpeg, MSM8998_MASTER_JPEG, 32, 0, 7, -1, true, NOC_QOS_MODE_BYPASS, 0, 7, 0, mas_mnoc_bimc_links);
DEFINE_QNODE(mas_mdp_p0, MSM8998_MASTER_MDP_P0, 32, 0, 8, -1, true, NOC_QOS_MODE_BYPASS, 0, 1, 25, mas_mnoc_bimc_links);
DEFINE_QNODE(mas_mdp_p1, MSM8998_MASTER_MDP_P1, 32, 0, 61, -1, true, NOC_QOS_MODE_BYPASS, 0, 2, 25, mas_mnoc_bimc_links);
DEFINE_QNODE(mas_rotator, MSM8998_MASTER_ROTATOR, 32, 0, 120, -1, true, NOC_QOS_MODE_BYPASS, 0, 0, 0, mas_mnoc_bimc_links);
/*
 * Venus spans QoS ports 3 and 4 under a single RPM master ID (9). Expose P0 for
 * RPM voting and keep P1 (-1/-1) to program the second hardware QoS port.
 */
DEFINE_QNODE(mas_venus, MSM8998_MASTER_VIDEO_P0, 32, 2, 9, -1, true, NOC_QOS_MODE_BYPASS, 0, 3, 0, mas_mnoc_bimc_links);
DEFINE_QNODE(mas_venus_p1, MSM8998_MASTER_VIDEO_P1, 32, 0, -1, -1, true, NOC_QOS_MODE_BYPASS, 0, 4, 0, mas_mnoc_bimc_links);
DEFINE_QNODE(mas_vfe, MSM8998_MASTER_VFE, 32, 0, 11, -1, true, NOC_QOS_MODE_BYPASS, 0, 6, 0, mas_mnoc_bimc_links);
DEFINE_QNODE(mas_venus_vmem, MSM8998_MASTER_VENUS_VMEM, 32, 0, 121, -1, true, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_venus_vmem_links);
DEFINE_QNODE_ENDP(slv_camera_cfg, MSM8998_SLAVE_CAMERA_CFG, 8, 0, 3, true);
DEFINE_QNODE_ENDP(slv_camera_throttle_cfg, MSM8998_SLAVE_CAMERA_THROTTLE_CFG, 8, 0, 154, true);
DEFINE_QNODE_ENDP(slv_misc_cfg, MSM8998_SLAVE_MISC_CFG, 8, 0, 8, true);
DEFINE_QNODE_ENDP(slv_venus_throttle_cfg, MSM8998_SLAVE_VENUS_THROTTLE_CFG, 8, 0, 178, true);
DEFINE_QNODE_ENDP(slv_venus_cfg, MSM8998_SLAVE_VENUS_CFG, 8, 0, 10, true);
DEFINE_QNODE_ENDP(slv_vmem_cfg, MSM8998_SLAVE_VMEM_CFG, 8, 0, 180, true);
DEFINE_QNODE_ENDP(slv_mmss_clk_xpu_cfg, MSM8998_SLAVE_MMSS_CLK_XPU_CFG, 8, 0, 13, true);
DEFINE_QNODE_ENDP(slv_mmss_clk_cfg, MSM8998_SLAVE_MMSS_CLK_CFG, 8, 0, 12, true);
DEFINE_QNODE_ENDP(slv_display_cfg, MSM8998_SLAVE_DISPLAY_CFG, 8, 0, 4, true);
DEFINE_QNODE_ENDP(slv_display_throttle_cfg, MSM8998_SLAVE_DISPLAY_THROTTLE_CFG, 8, 0, 156, true);
DEFINE_QNODE_ENDP(slv_smmu_cfg, MSM8998_SLAVE_SMMU_CFG, 8, 0, 205, true);
DEFINE_QNODE(slv_mnoc_bimc, MSM8998_SLAVE_MNOC_BIMC, 32, 0, -1, 16, true, NOC_QOS_MODE_INVALID, 0, -1, 0, slv_mnoc_bimc_links);
DEFINE_QNODE_ENDP(slv_vmem, MSM8998_SLAVE_VMEM, 32, 0, 179, true);
DEFINE_QNODE_ENDP(slv_srvc_mnoc, MSM8998_SLAVE_SRVC_MNOC, 8, 0, 17, true);

/* SNOC nodes */
DEFINE_QNODE(mas_hmss, MSM8998_MASTER_HMSS, 16, 0, 118, -1, true, NOC_QOS_MODE_FIXED, 1, 3, 0, mas_hmss_links);
DEFINE_QNODE(mas_qdss_bam, MSM8998_MASTER_QDSS_BAM, 16, 0, 19, -1, true, NOC_QOS_MODE_FIXED, 1, 1, 0, mas_qdss_snoc_links);
DEFINE_QNODE(mas_snoc_cfg, MSM8998_MASTER_SNOC_CFG, 16, 0, 20, -1, false, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_snoc_cfg_links);
DEFINE_QNODE(mas_bimc_snoc_0, MSM8998_MASTER_BIMC_SNOC_0, 16, 0, 21, -1, false, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_bimc_snoc_0_links);
DEFINE_QNODE(mas_bimc_snoc_1, MSM8998_MASTER_BIMC_SNOC_1, 16, 0, 109, -1, true, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_bimc_snoc_1_links);
DEFINE_QNODE(mas_a1noc_snoc, MSM8998_MASTER_A1NOC_SNOC, 16, 0, 111, -1, false, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_a1noc_snoc_links);
DEFINE_QNODE(mas_a2noc_snoc, MSM8998_MASTER_A2NOC_SNOC, 16, 0, 112, -1, false, NOC_QOS_MODE_INVALID, 0, -1, 0, mas_a2noc_snoc_links);
DEFINE_QNODE(mas_qdss_etr, MSM8998_MASTER_QDSS_ETR, 16, 0, 31, -1, true, NOC_QOS_MODE_FIXED, 1, 2, 0, mas_qdss_snoc_links);
DEFINE_QNODE_ENDP(slv_hmss, MSM8998_SLAVE_HMSS, 16, 0, 20, true);
DEFINE_QNODE_ENDP(slv_lpass, MSM8998_SLAVE_LPASS, 16, 0, 21, true);
DEFINE_QNODE_ENDP(slv_wlan, MSM8998_SLAVE_WLAN, 16, 0, 206, false);
DEFINE_QNODE(slv_snoc_bimc, MSM8998_SLAVE_SNOC_BIMC, 32, 0, -1, 24, false, NOC_QOS_MODE_INVALID, 0, -1, 0, slv_snoc_bimc_links);
DEFINE_QNODE(slv_snoc_cnoc, MSM8998_SLAVE_SNOC_CNOC, 16, 0, -1, 25, false, NOC_QOS_MODE_INVALID, 0, -1, 0, slv_snoc_cnoc_links);
DEFINE_QNODE_ENDP(slv_imem, MSM8998_SLAVE_IMEM, 16, 0, 26, false);
DEFINE_QNODE_ENDP(slv_pimem, MSM8998_SLAVE_PIMEM, 16, 0, 166, false);
DEFINE_QNODE_ENDP(slv_qdss_stm, MSM8998_SLAVE_QDSS_STM, 16, 0, 30, false);
DEFINE_QNODE_ENDP(slv_pcie_0, MSM8998_SLAVE_PCIE_0, 16, 0, 84, true);
DEFINE_QNODE_ENDP(slv_srvc_snoc, MSM8998_SLAVE_SRVC_SNOC, 16, 0, 29, false);

static struct qcom_icc_node * const msm8998_a1noc_nodes[] = {
	[MASTER_PCIE_0] = &mas_pcie_0,
	[MASTER_USB3] = &mas_usb3,
	[MASTER_UFS] = &mas_ufs,
	[MASTER_BLSP_2] = &mas_blsp_2,
	[SLAVE_A1NOC_SNOC] = &slv_a1noc_snoc,
};

static const struct regmap_config msm8998_a1noc_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0x5000,
	.fast_io	= true,
};

static const struct qcom_icc_desc msm8998_a1noc = {
	.type = QCOM_ICC_NOC,
	.nodes = msm8998_a1noc_nodes,
	.num_nodes = ARRAY_SIZE(msm8998_a1noc_nodes),
	.bus_clk_desc = &aggre1_clk,
	.intf_clocks = a1noc_intf_clocks,
	.num_intf_clocks = ARRAY_SIZE(a1noc_intf_clocks),
	.regmap_cfg = &msm8998_a1noc_regmap_config,
};

static struct qcom_icc_node * const msm8998_a2noc_nodes[] = {
	[MASTER_CNOC_A2NOC] = &mas_cnoc_a2noc,
	[MASTER_IPA] = &mas_ipa,
	[MASTER_SDCC_2] = &mas_sdcc_2,
	[MASTER_SDCC_4] = &mas_sdcc_4,
	[MASTER_TSIF] = &mas_tsif,
	[MASTER_BLSP_1] = &mas_blsp_1,
	[MASTER_CRVIRT_A2NOC] = &mas_cr_virt_a2noc,
	[MASTER_CRYPTO_CORE0] = &mas_crypto_c0,
	[SLAVE_CRVIRT_A2NOC] = &slv_cr_virt_a2noc,
	[SLAVE_A2NOC_SNOC] = &slv_a2noc_snoc,
};

static const struct regmap_config msm8998_a2noc_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0xa000,
	.fast_io	= true,
};

static const struct qcom_icc_desc msm8998_a2noc = {
	.type = QCOM_ICC_NOC,
	.nodes = msm8998_a2noc_nodes,
	.num_nodes = ARRAY_SIZE(msm8998_a2noc_nodes),
	.bus_clk_desc = &aggre2_clk,
	.intf_clocks = a2noc_intf_clocks,
	.num_intf_clocks = ARRAY_SIZE(a2noc_intf_clocks),
	.regmap_cfg = &msm8998_a2noc_regmap_config,
};

static struct qcom_icc_node * const msm8998_bimc_nodes[] = {
	[MASTER_GNOC_BIMC] = &mas_gnoc_bimc,
	[MASTER_OXILI] = &mas_oxili,
	[MASTER_MNOC_BIMC] = &mas_mnoc_bimc,
	[MASTER_SNOC_BIMC] = &mas_snoc_bimc,
	[SLAVE_EBI] = &slv_ebi,
	[SLAVE_HMSS_L3] = &slv_hmss_l3,
	[SLAVE_BIMC_SNOC_0] = &slv_bimc_snoc_0,
	[SLAVE_BIMC_SNOC_1] = &slv_bimc_snoc_1,
};

static const struct regmap_config msm8998_bimc_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0x5a000,
	.fast_io	= true,
};

static const struct qcom_icc_desc msm8998_bimc = {
	.type = QCOM_ICC_BIMC,
	.nodes = msm8998_bimc_nodes,
	.num_nodes = ARRAY_SIZE(msm8998_bimc_nodes),
	.bus_clk_desc = &bimc_clk,
	.regmap_cfg = &msm8998_bimc_regmap_config,
	.ab_coeff = 153,
	.keep_alive = true,
	.shared_mmio = true,
};

static struct qcom_icc_node * const msm8998_cnoc_nodes[] = {
	[MASTER_SNOC_CNOC] = &mas_snoc_cnoc,
	[MASTER_QDSS_DAP] = &mas_qdss_dap,
	[SLAVE_CNOC_A2NOC] = &slv_cnoc_a2noc,
	[SLAVE_SSC_CFG] = &slv_ssc_cfg,
	[SLAVE_MPM] = &slv_mpm,
	[SLAVE_PMIC_ARB] = &slv_pmic_arb,
	[SLAVE_TLMM_NORTH] = &slv_tlmm_north,
	[SLAVE_PIMEM_CFG] = &slv_pimem_cfg,
	[SLAVE_IMEM_CFG] = &slv_imem_cfg,
	[SLAVE_MESSAGE_RAM] = &slv_message_ram,
	[SLAVE_SKL] = &slv_skl,
	[SLAVE_BIMC_CFG] = &slv_bimc_cfg,
	[SLAVE_PRNG] = &slv_prng,
	[SLAVE_A2NOC_CFG] = &slv_a2noc_cfg,
	[SLAVE_IPA_CFG] = &slv_ipa_cfg,
	[SLAVE_TCSR] = &slv_tcsr,
	[SLAVE_SNOC_CFG] = &slv_snoc_cfg,
	[SLAVE_CLK_CTL] = &slv_clk_ctl,
	[SLAVE_GLM] = &slv_glm,
	[SLAVE_SPDM] = &slv_spdm,
	[SLAVE_GPUSS_CFG] = &slv_gpuss_cfg,
	[SLAVE_CNOC_MNOC_CFG] = &slv_cnoc_mnoc_cfg,
	[SLAVE_QM_CFG] = &slv_qm_cfg,
	[SLAVE_MSS_CFG] = &slv_mss_cfg,
	[SLAVE_UFS_CFG] = &slv_ufs_cfg,
	[SLAVE_TLMM_WEST] = &slv_tlmm_west,
	[SLAVE_A1NOC_CFG] = &slv_a1noc_cfg,
	[SLAVE_AHB2PHY] = &slv_ahb2phy,
	[SLAVE_BLSP_2] = &slv_blsp_2,
	[SLAVE_PDM] = &slv_pdm,
	[SLAVE_USB3_0] = &slv_usb3_0,
	[SLAVE_A1NOC_SMMU_CFG] = &slv_a1noc_smmu_cfg,
	[SLAVE_BLSP_1] = &slv_blsp_1,
	[SLAVE_SDCC_2] = &slv_sdcc_2,
	[SLAVE_SDCC_4] = &slv_sdcc_4,
	[SLAVE_TSIF] = &slv_tsif,
	[SLAVE_QDSS_CFG] = &slv_qdss_cfg,
	[SLAVE_TLMM_EAST] = &slv_tlmm_east,
	[SLAVE_CNOC_MNOC_MMSS_CFG] = &slv_cnoc_mnoc_mmss_cfg,
	[SLAVE_SRVC_CNOC] = &slv_srvc_cnoc,
};

static const struct qcom_icc_desc msm8998_cnoc = {
	.type = QCOM_ICC_NOC,
	.nodes = msm8998_cnoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8998_cnoc_nodes),
	.bus_clk_desc = &bus_2_clk,
	.keep_alive = true,
};

static struct qcom_icc_node * const msm8998_gnoc_nodes[] = {
	[MASTER_APPS_PROC] = &mas_apps_proc,
	[SLAVE_GNOC_BIMC] = &slv_gnoc_bimc,
};

static const struct qcom_icc_desc msm8998_gnoc = {
	.type = QCOM_ICC_NOC,
	.nodes = msm8998_gnoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8998_gnoc_nodes),
};

static struct qcom_icc_node * const msm8998_mnoc_nodes[] = {
	[MASTER_CNOC_MNOC_MMSS_CFG] = &mas_cnoc_mnoc_mmss_cfg,
	[MASTER_CNOC_MNOC_CFG] = &mas_cnoc_mnoc_cfg,
	[MASTER_CPP] = &mas_cpp,
	[MASTER_JPEG] = &mas_jpeg,
	[MASTER_MDP_P0] = &mas_mdp_p0,
	[MASTER_MDP_P1] = &mas_mdp_p1,
	[MASTER_ROTATOR] = &mas_rotator,
	[MASTER_VIDEO_P0] = &mas_venus,
	[MASTER_VIDEO_P1] = &mas_venus_p1,
	[MASTER_VFE] = &mas_vfe,
	[MASTER_VENUS_VMEM] = &mas_venus_vmem,
	[SLAVE_CAMERA_CFG] = &slv_camera_cfg,
	[SLAVE_CAMERA_THROTTLE_CFG] = &slv_camera_throttle_cfg,
	[SLAVE_MISC_CFG] = &slv_misc_cfg,
	[SLAVE_VENUS_THROTTLE_CFG] = &slv_venus_throttle_cfg,
	[SLAVE_VENUS_CFG] = &slv_venus_cfg,
	[SLAVE_VMEM_CFG] = &slv_vmem_cfg,
	[SLAVE_MMSS_CLK_XPU_CFG] = &slv_mmss_clk_xpu_cfg,
	[SLAVE_MMSS_CLK_CFG] = &slv_mmss_clk_cfg,
	[SLAVE_DISPLAY_CFG] = &slv_display_cfg,
	[SLAVE_DISPLAY_THROTTLE_CFG] = &slv_display_throttle_cfg,
	[SLAVE_SMMU_CFG] = &slv_smmu_cfg,
	[SLAVE_MNOC_BIMC] = &slv_mnoc_bimc,
	[SLAVE_VMEM] = &slv_vmem,
	[SLAVE_SRVC_MNOC] = &slv_srvc_mnoc,
};

static const struct regmap_config msm8998_mnoc_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0x9000,
	.fast_io	= true,
};

static const struct qcom_icc_desc msm8998_mnoc = {
	.type = QCOM_ICC_NOC,
	.nodes = msm8998_mnoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8998_mnoc_nodes),
	.bus_clk_desc = &mmaxi_0_clk,
	.intf_clocks = mnoc_intf_clocks,
	.num_intf_clocks = ARRAY_SIZE(mnoc_intf_clocks),
	.regmap_cfg = &msm8998_mnoc_regmap_config,
	.ab_coeff = 153,
};

static struct qcom_icc_node * const msm8998_snoc_nodes[] = {
	[MASTER_HMSS] = &mas_hmss,
	[MASTER_QDSS_BAM] = &mas_qdss_bam,
	[MASTER_SNOC_CFG] = &mas_snoc_cfg,
	[MASTER_BIMC_SNOC_0] = &mas_bimc_snoc_0,
	[MASTER_BIMC_SNOC_1] = &mas_bimc_snoc_1,
	[MASTER_A1NOC_SNOC] = &mas_a1noc_snoc,
	[MASTER_A2NOC_SNOC] = &mas_a2noc_snoc,
	[MASTER_QDSS_ETR] = &mas_qdss_etr,
	[SLAVE_HMSS] = &slv_hmss,
	[SLAVE_LPASS] = &slv_lpass,
	[SLAVE_WLAN] = &slv_wlan,
	[SLAVE_SNOC_BIMC] = &slv_snoc_bimc,
	[SLAVE_SNOC_CNOC] = &slv_snoc_cnoc,
	[SLAVE_IMEM] = &slv_imem,
	[SLAVE_PIMEM] = &slv_pimem,
	[SLAVE_QDSS_STM] = &slv_qdss_stm,
	[SLAVE_PCIE_0] = &slv_pcie_0,
	[SLAVE_SRVC_SNOC] = &slv_srvc_snoc,
};

static const struct regmap_config msm8998_snoc_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0x4000,
	.fast_io	= true,
};

static const struct qcom_icc_desc msm8998_snoc = {
	.type = QCOM_ICC_NOC,
	.nodes = msm8998_snoc_nodes,
	.num_nodes = ARRAY_SIZE(msm8998_snoc_nodes),
	.bus_clk_desc = &bus_1_clk,
	.regmap_cfg = &msm8998_snoc_regmap_config,
	.keep_alive = true,
};

static const struct of_device_id msm8998_noc_of_match[] = {
	{ .compatible = "qcom,msm8998-a1noc", .data = &msm8998_a1noc },
	{ .compatible = "qcom,msm8998-a2noc", .data = &msm8998_a2noc },
	{ .compatible = "qcom,msm8998-bimc", .data = &msm8998_bimc },
	{ .compatible = "qcom,msm8998-cnoc", .data = &msm8998_cnoc },
	{ .compatible = "qcom,msm8998-gnoc", .data = &msm8998_gnoc },
	{ .compatible = "qcom,msm8998-mnoc", .data = &msm8998_mnoc },
	{ .compatible = "qcom,msm8998-snoc", .data = &msm8998_snoc },
	{ }
};
MODULE_DEVICE_TABLE(of, msm8998_noc_of_match);

static struct platform_driver msm8998_noc_driver = {
	.probe = qnoc_probe,
	.remove = qnoc_remove,
	.driver = {
		.name = "qnoc-msm8998",
		.of_match_table = msm8998_noc_of_match,
		.sync_state = icc_sync_state,
	},
};
module_platform_driver(msm8998_noc_driver);

MODULE_DESCRIPTION("Qualcomm MSM8998 NoC interconnect driver");
MODULE_LICENSE("GPL");
