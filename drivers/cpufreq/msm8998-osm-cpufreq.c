// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clean-room MSM8998 Operating State Manager (OSM v2) + CPRh + SAW4 AVS
 * CPUFreq driver for OnePlus 5T (dumpling), ported from MSM8998 v2 hardware
 * specifications.
 */

#define pr_fmt(fmt) "msm8998-osm-cpufreq: " fmt

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/topology.h>

#include "msm8998-osm-tables.h"

static bool probe_only;
module_param(probe_only, bool, 0444);
MODULE_PARM_DESC(probe_only, "Read fuses and log computed tables without programming HW or registering cpufreq");

static bool closed_loop = true;
module_param(closed_loop, bool, 0444);
MODULE_PARM_DESC(closed_loop, "Enable CPRh full HW closed-loop AVS (default true)");

#define MSM8998_NUM_CLUSTERS			2
#define MSM8998_CLUSTER_PWR			0
#define MSM8998_CLUSTER_PERF			1

#define CPR3_RO_COUNT				16
#define CPR3_RO_MASK				GENMASK(15, 0)
#define CPRH_FUSE_CORNERS			4
#define CPRH_FUSE_STEP_VOLT_UV			10000
#define CPRH_VOLT_FUSE_LEN			6
#define CPRH_QUOT_OFFSET_SCALE			5
#define CPRH_STEP_VOLT_UV			4000
#define CPRH_BASE_VOLT_UV			352000
#define CPRH_DELTA_QUOT_STEP_FACTOR		4
#define CPRH_MODE_SWITCH_DELAY_FACTOR		4
#define CPRH_CPR_CLK_RATE_HZ			19200000U
#define CPRH_MAX_CORNERS			40

#define CPRH_APM_THRESHOLD_UV			800000
#define CPRH_APM_CROSSOVER_UV			880000
#define CPRH_MEM_ACC_THRESHOLD_UV		852000
#define CPRH_MEM_ACC_CROSSOVER_UV		852000

/* SAW4 AVS registers */
#define SAW4_REG_AVS_CTL			0x904
#define SAW4_REG_AVS_LIMIT			0x908
#define SAW4_AVS_CTL_VAL			0x01010031U

/* CPRh registers */
#define CPR3_REG_CPR_CTL			0x4
#define CPR3_CPR_CTL_LOOP_EN_MASK		BIT(0)
#define CPR3_CPR_CTL_IDLE_CLOCKS_SHIFT		1
#define CPR3_CPR_CTL_COUNT_MODE_SHIFT		6
#define CPR3_CPR_CTL_COUNT_REPEAT_SHIFT		9

#define CPR3_REG_CPR_STATUS			0x8
#define CPR3_REG_CPR_TIMER_AUTO_CONT		0xC
#define CPR3_REG_CPR_STEP_QUOT			0x14
#define CPR3_REG_GCNT(ro)			(0xA0 + 0x4 * (ro))
#define CPR3_REG_SENSOR_OWNER(sensor)		(0x200 + 0x4 * (sensor))

#define CPR4_REG_SAW_ERROR_STEP_LIMIT		0x7A4
#define CPR4_REG_MARGIN_TEMP_CORE_TIMERS	0x7A8
#define CPR4_REG_MARGIN_ADJ_CTL			0x7F8
#define CPR4_MARGIN_ADJ_CTL_HW_CL_EN		BIT(4)
#define CPR4_MARGIN_ADJ_CTL_PER_RO_KV_EN	BIT(7)
#define CPR4_MARGIN_ADJ_CTL_PMIC_STEP_SHIFT	12
#define CPR4_MARGIN_TEMP_CORE_TIMERS_SETTLE_MASK GENMASK(28, 18)
#define CPR4_MARGIN_TEMP_CORE_TIMERS_SETTLE_SHIFT 18

#define CPR3_REG_THRESH(thread)			(0x808 + 0x440 * (thread))
#define CPR3_REG_RO_MASK(thread)		(0x80C + 0x440 * (thread))
#define CPR3_REG_TARGET_QUOT(thread, ro)	(0x840 + 0x440 * (thread) + 0x4 * (ro))

#define CPRH_REG_CORNER(corner)			(0x3A00 + 0x4 * (corner))
#define CPRH_CORNER_INIT_VOLT_MASK		GENMASK(7, 0)
#define CPRH_CORNER_FLOOR_VOLT_MASK		GENMASK(15, 8)
#define CPRH_CORNER_FLOOR_VOLT_SHIFT		8
#define CPRH_CORNER_QUOT_DELTA_MASK		GENMASK(24, 16)
#define CPRH_CORNER_QUOT_DELTA_SHIFT		16
#define CPRH_CORNER_RO_SEL_MASK			GENMASK(28, 25)
#define CPRH_CORNER_RO_SEL_SHIFT		25
#define CPRH_CORNER_CPR_CL_DISABLE		BIT(29)

#define CPRH_REG_CTL				0x3AA0
#define CPRH_CTL_OSM_ENABLED			BIT(0)
#define CPRH_CTL_BASE_VOLTAGE_MASK		GENMASK(10, 1)
#define CPRH_CTL_BASE_VOLTAGE_SHIFT		1
#define CPRH_CTL_MODE_SWITCH_DELAY_MASK		GENMASK(24, 17)
#define CPRH_CTL_MODE_SWITCH_DELAY_SHIFT	17
#define CPRH_CTL_VOLT_MULT_MASK			GENMASK(28, 25)
#define CPRH_CTL_VOLT_MULT_SHIFT		25
#define CPRH_REG_STATUS				0x3AA4

/* OSM v2 registers */
#define OSM_BANK_STRIDE				0x2000
#define OSM_VERSION_REG				0x0
#define OSM_SEQ_REG(n)				(0x300 + (n) * 4)
#define OSM_ENABLE_REG				0x1004
#define OSM_CC_ZERO_BEHAV_CTRL			0x100C
#define OSM_SPM_CC_HYSTERESIS			0x101C
#define OSM_SPM_CC_DCVS_DISABLE			0x1020
#define OSM_SPM_CORE_RET_MAPPING		0x1024
#define OSM_LLM_FREQ_VOTE_HYST			0x102C
#define OSM_LLM_VOLT_VOTE_HYST			0x1030
#define OSM_LLM_INTF_DCVS_DISABLE		0x1034
#define OSM_SEQ_REG1_V2				0x1048
#define OSM_PDN_FSM_CTRL_REG			0x1070
#define OSM_CC_BOOST_TIMER_REG0			0x1074
#define OSM_CC_BOOST_TIMER_REG1			0x1078
#define OSM_CC_BOOST_TIMER_REG2			0x107C
#define OSM_DCVS_BOOST_TIMER_REG0		0x1084
#define OSM_DCVS_BOOST_TIMER_REG1		0x1088
#define OSM_DCVS_BOOST_TIMER_REG2		0x108C
#define OSM_PS_BOOST_TIMER_REG0			0x1094
#define OSM_PS_BOOST_TIMER_REG1			0x1098
#define OSM_PS_BOOST_TIMER_REG2			0x109C
#define OSM_BOOST_PROG_SYNC_DELAY_REG		0x10A0
#define OSM_DROOP_CTRL_REG			0x10A4
#define OSM_DROOP_RELEASE_TIMER_CTRL		0x10A8
#define OSM_DROOP_UNSTALL_TIMER_CTRL		0x10AC
#define OSM_DROOP_WAIT_TO_REL_CTRL0		0x10B0
#define OSM_DCVS_DROOP_TIMER_CTRL		0x10B8
#define OSM_DROOP_PROG_SYNC_DELAY_REG		0x10BC
#define OSM_PLL_SW_OVERRIDE_EN			0x10C0

#define OSM_TABLE_SIZE				40
#define OSM_MAX_VIRTUAL_CORNER			(OSM_TABLE_SIZE - 1)
#define OSM_SEQ_MINUS_ONE			0xFF
#define OSM_REG_STRIDE				32
#define OSM_INDEX_REG				0x1150
#define OSM_FREQ_REG				0x1154
#define OSM_VOLT_REG				0x1158
#define OSM_OVERRIDE_REG			0x115C
#define OSM_SPARE_REG				0x1164

#define OSM_WDOG_PSTATE_STATUS			0x1C00
#define OSM_CYCLE_COUNTER_CTRL			0x1F00
#define OSM_CYCLE_COUNTER_STATUS		0x1F04
#define OSM_DCVS_PERF_STATE_DESIRED		0x1F10

#define OSM_CLK_RATE_HZ				200000000U
#define OSM_XO_RATE_HZ				19200000U
#define OSM_AUX_RATE_HZ				300000000U
#define OSM_MAX_CORE_COUNT			4

/* APCS common LMh RCGR */
#define APCS_COMMON_LMH_CMD_RCGR		0x12C
#define APCS_COMMON_LMH_CFG_RCGR		0x130

/* ACD registers */
#define ACD_HW_VERSION				0x0
#define ACDCR					0x4
#define ACDTD					0x8
#define ACDSSCR					0x28
#define ACD_EXTINT_CFG				0x30
#define ACD_DCVS_SW				0x34
#define ACD_GFMUX_CFG				0x3C
#define ACD_AUTOXFER_CFG			0x80
#define ACD_AUTOXFER				0x84
#define ACD_AUTOXFER_CTL			0x88
#define ACD_AUTOXFER_STATUS			0x8C
#define ACD_WRITE_CTL				0x90
#define ACD_WRITE_STATUS			0x94
#define ACD_LOCAL_XFER_TIMEOUT_NS		500

#define ACD_BITMASK(off)			BIT((off) / 4)

struct cpr3_fuse_seg {
	u8 row;
	u8 bit_start;
	u8 bit_end;
};

struct msm8998_cpr_corner {
	u32 proc_freq_hz;
	u32 floor_uv;
	u32 ceiling_uv;
	u32 open_loop_uv;
	u32 target_quot[CPR3_RO_COUNT];
	u8 fuse_corner;
	u8 ro_sel;
	bool use_open_loop;
};

struct msm8998_cpr_fuses {
	u32 speed_bin;
	u32 cpr_fusing_rev;
	u32 fuse_combo;
	u32 force_highest;
	u32 ro_sel[CPRH_FUSE_CORNERS];
	u32 init_volt[CPRH_FUSE_CORNERS];
	u32 target_quot[CPRH_FUSE_CORNERS];
	u32 quot_offset[CPRH_FUSE_CORNERS];
};

struct msm8998_cluster {
	u32 id;
	const char *name;
	phys_addr_t osm_phys;
	void __iomem *osm_base;
	void __iomem *acd_base;
	void __iomem *cprh_base;
	void __iomem *saw4_base;
	struct cpumask cpus;

	struct msm8998_cpr_fuses fuses;
	struct msm8998_cpr_corner corners[CPRH_MAX_CORNERS];
	u32 func_corner_count;
	u32 total_corner_count;
	u32 base_quots[CPR3_RO_COUNT];

	const struct msm8998_osm_lut_row *lut;
	u32 lut_len;
	u32 opp_ol_mv[OSM_TABLE_SIZE];
	u32 boot_rate_hz;

	u32 apm_threshold_vc;
	u32 apm_threshold_pre_vc;
	u32 apm_crossover_vc;
	u32 mem_acc_threshold_vc;
	u32 mem_acc_threshold_pre_vc;
	u32 mem_acc_crossover_vc;

	/* Hardware config constants */
	u32 saw4_avs_limit;
	u32 cpr_sensor_count;
	u32 step_quot_min;
	u32 step_quot_max;
};

struct msm8998_osm_drvdata {
	struct device *dev;
	void __iomem *qfprom_base;
	void __iomem *apcs_common_base;
	struct clk *aux_clk;
	struct clk *rbcpr_clk;
	struct msm8998_cluster cl[MSM8998_NUM_CLUSTERS];
	bool clks_enabled;
};

static struct msm8998_osm_drvdata *g_osm_data;

/* MSM8998 QFPROM fuse layout (LSB-first concatenation for multi-segment fields) */
static const struct cpr3_fuse_seg seg_speed_bin[] = { {38, 29, 31}, {} };
static const struct cpr3_fuse_seg seg_cpr_rev[]   = { {39, 51, 53}, {} };
static const struct cpr3_fuse_seg seg_force_hi[]  = { {100, 45, 45}, {} };

static const struct cpr3_fuse_seg seg_ro_sel[2][CPRH_FUSE_CORNERS][2] = {
	[MSM8998_CLUSTER_PWR] = {
		{ {67, 12, 15}, {} }, { {67,  8, 11}, {} },
		{ {67,  4,  7}, {} }, { {67,  0,  3}, {} },
	},
	[MSM8998_CLUSTER_PERF] = {
		{ {69, 26, 29}, {} }, { {69, 22, 25}, {} },
		{ {69, 18, 21}, {} }, { {69, 14, 17}, {} },
	},
};

static const struct cpr3_fuse_seg seg_init_volt[2][CPRH_FUSE_CORNERS][2] = {
	[MSM8998_CLUSTER_PWR] = {
		{ {67, 34, 39}, {} }, { {67, 28, 33}, {} },
		{ {67, 22, 27}, {} }, { {67, 16, 21}, {} },
	},
	[MSM8998_CLUSTER_PERF] = {
		{ {69, 48, 53}, {} }, { {69, 42, 47}, {} },
		{ {69, 36, 41}, {} }, { {69, 30, 35}, {} },
	},
};

static const struct cpr3_fuse_seg seg_target_quot[2][CPRH_FUSE_CORNERS][3] = {
	[MSM8998_CLUSTER_PWR] = {
		{ {68, 18, 29}, {} },
		{ {68,  6, 17}, {} },
		{ {67, 58, 63}, {68,  0,  5}, {} },
		{ {67, 46, 57}, {} },
	},
	[MSM8998_CLUSTER_PERF] = {
		{ {70, 32, 43}, {} },
		{ {70, 20, 31}, {} },
		{ {70,  8, 19}, {} },
		{ {69, 60, 63}, {70,  0,  7}, {} },
	},
};

static const struct cpr3_fuse_seg seg_quot_offset[2][CPRH_FUSE_CORNERS][3] = {
	[MSM8998_CLUSTER_PWR] = {
		{ {} },
		{ {68, 63, 63}, {69,  0,  5}, {} },
		{ {68, 56, 62}, {} },
		{ {68, 49, 55}, {} },
	},
	[MSM8998_CLUSTER_PERF] = {
		{ {} },
		{ {71, 13, 15}, {71, 21, 24}, {} },
		{ {71,  6, 12}, {} },
		{ {70, 63, 63}, {71,  0,  5}, {} },
	},
};

static const int fuse_ref_volt_v2[2][CPRH_FUSE_CORNERS] = {
	[MSM8998_CLUSTER_PWR]  = { 688000, 756000, 828000, 1056000 },
	[MSM8998_CLUSTER_PERF] = { 756000, 756000, 828000, 1056000 },
};

static u32 read_fuse_param(void __iomem *qfprom_base,
			   const struct cpr3_fuse_seg *segs)
{
	u32 val = 0, bits_total = 0;

	while (segs->bit_end > 0 || segs->row > 0) {
		u32 bits = segs->bit_end - segs->bit_start + 1;
		u64 row_val;
		u32 lo = readl_relaxed(qfprom_base + segs->row * 8);
		u32 hi = readl_relaxed(qfprom_base + segs->row * 8 + 4);
		u64 mask = GENMASK_ULL(segs->bit_end, segs->bit_start);

		row_val = (((u64)hi << 32) | lo) & mask;
		row_val >>= segs->bit_start;
		/* Multi-segment QFPROM parameters concatenate LSB-first */
		val |= ((u32)row_val) << bits_total;
		bits_total += bits;
		segs++;
	}

	return val;
}

static int convert_open_loop_fuse(int ref_volt, u32 fuse)
{
	int sign = (fuse & BIT(CPRH_VOLT_FUSE_LEN - 1)) ? -1 : 1;
	int steps = fuse & (BIT(CPRH_VOLT_FUSE_LEN - 1) - 1);

	return ref_volt + sign * steps * CPRH_FUSE_STEP_VOLT_UV;
}

static u64 cpr3_interpolate(u64 x1, u64 y1, u64 x2, u64 y2, u64 x)
{
	u64 temp;

	if (x1 >= x2 || y1 > y2 || x1 > x || x > x2)
		return y2;

	temp = (x2 - x) * (y2 - y1);
	do_div(temp, (u32)(x2 - x1));
	return y2 - temp;
}

static int cpr3_quot_adjustment(int ro_scale, int volt_adjust)
{
	u64 temp;
	int sign = 1;

	if (ro_scale < 0) {
		sign = -sign;
		ro_scale = -ro_scale;
	}
	if (volt_adjust < 0) {
		sign = -sign;
		volt_adjust = -volt_adjust;
	}

	temp = (u64)ro_scale * (u64)volt_adjust;
	do_div(temp, 1000000U);
	return sign * (int)temp;
}

static u32 round_up_step(u32 volt_uv)
{
	return DIV_ROUND_UP(volt_uv, CPRH_STEP_VOLT_UV) * CPRH_STEP_VOLT_UV;
}

static void adjust_voltages_for_threshold(struct msm8998_cluster *c,
					  u32 threshold_uv)
{
	u32 i;

	for (i = 0; i < c->func_corner_count; i++) {
		struct msm8998_cpr_corner *corner = &c->corners[i];

		if (threshold_uv <= corner->floor_uv ||
		    threshold_uv > corner->ceiling_uv)
			continue;

		if (corner->open_loop_uv >= threshold_uv) {
			corner->floor_uv = max(corner->floor_uv, threshold_uv);
			if (corner->open_loop_uv < corner->floor_uv)
				corner->open_loop_uv = corner->floor_uv;
		} else {
			corner->ceiling_uv = threshold_uv - CPRH_STEP_VOLT_UV;
		}
	}
}

static int compute_cluster_cprh(struct msm8998_osm_drvdata *d,
				struct msm8998_cluster *c)
{
	struct msm8998_cpr_fuses *f = &c->fuses;
	const u32 *fmax_map, *ceiling_tbl, *floor_tbl, *max_range_tbl;
	const u32 *freq_tbl, *ro_scale_tbl;
	const s32 *ol_adj_tbl, *cl_adj_tbl, *ol_fuse_adj, *cl_fuse_adj;
	u32 tbl_off = 0, adj_off = 0, num_corners;
	u32 fmax_corner[CPRH_FUSE_CORNERS];
	int fuse_volt[CPRH_FUSE_CORNERS];
	u64 quot_low[CPRH_FUSE_CORNERS], quot_high[CPRH_FUSE_CORNERS];
	u32 ro_scale[CPRH_FUSE_CORNERS];
	u32 i, j, prev_volt;

	f->speed_bin = read_fuse_param(d->qfprom_base, seg_speed_bin);
	f->cpr_fusing_rev = read_fuse_param(d->qfprom_base, seg_cpr_rev);
	f->force_highest = read_fuse_param(d->qfprom_base, seg_force_hi);
	f->fuse_combo = f->cpr_fusing_rev + 8 * f->speed_bin;

	if (f->speed_bin >= 4 || f->fuse_combo >= 32) {
		dev_err(d->dev, "%s: invalid fuses speed_bin=%u rev=%u combo=%u\n",
			c->name, f->speed_bin, f->cpr_fusing_rev, f->fuse_combo);
		return -EINVAL;
	}

	for (i = 0; i < CPRH_FUSE_CORNERS; i++) {
		f->ro_sel[i] = read_fuse_param(d->qfprom_base,
					       seg_ro_sel[c->id][i]);
		f->init_volt[i] = read_fuse_param(d->qfprom_base,
						  seg_init_volt[c->id][i]);
		f->target_quot[i] = read_fuse_param(d->qfprom_base,
						    seg_target_quot[c->id][i]);
		f->quot_offset[i] = read_fuse_param(d->qfprom_base,
						    seg_quot_offset[c->id][i]);
	}

	if (c->id == MSM8998_CLUSTER_PWR) {
		num_corners = msm8998_pwrcl_speedbin_corners[f->speed_bin];
		tbl_off = 0;
		adj_off = f->speed_bin * num_corners;
		fmax_map = msm8998_pwrcl_fmax_map;
		ceiling_tbl = msm8998_pwrcl_ceiling_uv;
		floor_tbl = msm8998_pwrcl_floor_uv;
		max_range_tbl = msm8998_pwrcl_max_range_uv;
		freq_tbl = msm8998_pwrcl_corner_freqs_hz;
		ro_scale_tbl = msm8998_pwrcl_ro_scale;
		ol_adj_tbl = msm8998_pwrcl_ol_adj_uv;
		cl_adj_tbl = msm8998_pwrcl_cl_adj_uv;
		ol_fuse_adj = msm8998_pwrcl_ol_fuse_adj_uv;
		cl_fuse_adj = msm8998_pwrcl_cl_fuse_adj_uv;
		c->lut = msm8998_pwrcl_lut_bin0;
		c->lut_len = ARRAY_SIZE(msm8998_pwrcl_lut_bin0);
	} else {
		static const struct {
			const struct msm8998_osm_lut_row *lut;
			u32 len;
		} perf_luts[4] = {
			{ msm8998_perfcl_lut_bin0, ARRAY_SIZE(msm8998_perfcl_lut_bin0) },
			{ msm8998_perfcl_lut_bin1, ARRAY_SIZE(msm8998_perfcl_lut_bin1) },
			{ msm8998_perfcl_lut_bin2, ARRAY_SIZE(msm8998_perfcl_lut_bin2) },
			{ msm8998_perfcl_lut_bin3, ARRAY_SIZE(msm8998_perfcl_lut_bin3) },
		};

		num_corners = msm8998_perfcl_speedbin_corners[f->speed_bin];
		for (i = 0; i < f->speed_bin; i++)
			tbl_off += msm8998_perfcl_speedbin_corners[i];
		adj_off = tbl_off;
		fmax_map = &msm8998_perfcl_fmax_map[f->speed_bin * CPRH_FUSE_CORNERS];
		ceiling_tbl = msm8998_perfcl_ceiling_uv;
		floor_tbl = msm8998_perfcl_floor_uv;
		max_range_tbl = msm8998_perfcl_max_range_uv;
		freq_tbl = msm8998_perfcl_corner_freqs_hz;
		ro_scale_tbl = msm8998_perfcl_ro_scale;
		ol_adj_tbl = msm8998_perfcl_ol_adj_uv;
		cl_adj_tbl = msm8998_perfcl_cl_adj_uv;
		ol_fuse_adj = msm8998_perfcl_ol_fuse_adj_uv;
		cl_fuse_adj = msm8998_perfcl_cl_fuse_adj_uv;
		c->lut = perf_luts[f->speed_bin].lut;
		c->lut_len = perf_luts[f->speed_bin].len;
	}

	c->func_corner_count = num_corners;
	for (i = 0; i < CPRH_FUSE_CORNERS; i++)
		fmax_corner[i] = fmax_map[i] - 1;

	/* Initialize per-corner base ceiling, floor, frequency, and fuse corner */
	for (i = 0; i < num_corners; i++) {
		u8 fc = 0;

		while (fc < CPRH_FUSE_CORNERS - 1 && i > fmax_corner[fc])
			fc++;

		c->corners[i].proc_freq_hz = freq_tbl[tbl_off + i];
		c->corners[i].ceiling_uv = ceiling_tbl[tbl_off + i];
		c->corners[i].floor_uv = floor_tbl[tbl_off + i];
		c->corners[i].fuse_corner = fc;
		c->corners[i].ro_sel = f->ro_sel[fc];
	}

	/* Compute fuse-corner open-loop voltages */
	for (i = 0; i < CPRH_FUSE_CORNERS; i++) {
		fuse_volt[i] = convert_open_loop_fuse(fuse_ref_volt_v2[c->id][i],
						      f->init_volt[i]);
		fuse_volt[i] += ol_fuse_adj[f->fuse_combo * CPRH_FUSE_CORNERS + i];
	}

	/* Populate functional corners via frequency interpolation */
	for (i = 0; i <= fmax_corner[0]; i++)
		c->corners[i].open_loop_uv = round_up_step(
			clamp_t(int, fuse_volt[0], c->corners[i].floor_uv,
				c->corners[i].ceiling_uv));

	for (i = 1; i < CPRH_FUSE_CORNERS; i++) {
		u32 fl = c->corners[fmax_corner[i - 1]].proc_freq_hz;
		u32 fh = c->corners[fmax_corner[i]].proc_freq_hz;
		int vl = max_t(int, fuse_volt[i - 1], 0);
		int vh = max(fuse_volt[i], vl);

		for (j = fmax_corner[i - 1] + 1; j <= fmax_corner[i]; j++) {
			u64 v = cpr3_interpolate(fl, vl, fh, vh,
						 c->corners[j].proc_freq_hz);
			c->corners[j].open_loop_uv = round_up_step((u32)v);
		}
	}

	/* Add per-corner OL adjustments, enforce monotonicity, clamp [floor, ceil] */
	prev_volt = 0;
	for (i = 0; i < num_corners; i++) {
		s32 v = (s32)c->corners[i].open_loop_uv + ol_adj_tbl[adj_off + i];

		if (i > 0 && v < (s32)prev_volt)
			v = prev_volt;
		v = clamp_t(s32, v, c->corners[i].floor_uv,
			    c->corners[i].ceiling_uv);
		c->corners[i].open_loop_uv = round_up_step((u32)v);
		prev_volt = c->corners[i].open_loop_uv;
	}

	/* Adjust thresholds for APM & MEM-ACC, set ceiling = OL, limit floors */
	adjust_voltages_for_threshold(c, CPRH_APM_THRESHOLD_UV);
	adjust_voltages_for_threshold(c, CPRH_MEM_ACC_THRESHOLD_UV);

	for (i = 0; i < num_corners; i++) {
		s32 range = (s32)max_range_tbl[tbl_off + i];
		u32 flr_min;

		c->corners[i].ceiling_uv = c->corners[i].open_loop_uv;
		if (range >= 0) {
			flr_min = round_up_step(c->corners[i].ceiling_uv - range);
			if (c->corners[i].floor_uv < flr_min)
				c->corners[i].floor_uv = flr_min;
		}
		if (c->corners[i].open_loop_uv < c->corners[i].floor_uv)
			c->corners[i].open_loop_uv = c->corners[i].floor_uv;
		if (i > 0 && c->corners[i].floor_uv < c->corners[i - 1].floor_uv)
			c->corners[i].floor_uv = c->corners[i - 1].floor_uv;
	}

	/* Compute CPRh target quotients with RO scaling & fuse adjustments */
	for (i = 0; i < CPRH_FUSE_CORNERS; i++)
		ro_scale[i] = ro_scale_tbl[i * CPR3_RO_COUNT + f->ro_sel[i]];

	{
		s32 qa0 = cpr3_quot_adjustment(ro_scale[0],
					       cl_fuse_adj[f->fuse_combo * CPRH_FUSE_CORNERS]);
		u32 q0 = (u32)max_t(s32, (s32)f->target_quot[0] + qa0, 1);

		quot_low[0] = quot_high[0] = q0;
		for (i = 0; i <= fmax_corner[0]; i++)
			c->corners[i].target_quot[f->ro_sel[0]] = q0;
	}

	for (i = 1; i < CPRH_FUSE_CORNERS; i++) {
		s32 qa_hi = cpr3_quot_adjustment(ro_scale[i],
						 cl_fuse_adj[f->fuse_combo * CPRH_FUSE_CORNERS + i]);
		quot_high[i] = f->target_quot[i];
		if (f->ro_sel[i] == f->ro_sel[i - 1])
			quot_low[i] = quot_high[i - 1];
		else
			quot_low[i] = quot_high[i] - (u64)f->quot_offset[i] * CPRH_QUOT_OFFSET_SCALE;
		if (quot_high[i] < quot_low[i])
			quot_high[i] = quot_low[i];

		quot_high[i] = (u64)max_t(s64, (s64)quot_high[i] + qa_hi, 1);
		if (f->ro_sel[i] == f->ro_sel[i - 1]) {
			quot_low[i] = quot_high[i - 1];
		} else {
			s32 qa_lo = cpr3_quot_adjustment(ro_scale[i],
							 cl_fuse_adj[f->fuse_combo * CPRH_FUSE_CORNERS + i - 1]);
			quot_low[i] = (u64)max_t(s64, (s64)quot_low[i] + qa_lo, 1);
		}
		if (quot_high[i] < quot_low[i])
			quot_high[i] = quot_low[i];
	}

	for (i = 1; i < CPRH_FUSE_CORNERS; i++) {
		u32 fl = c->corners[fmax_corner[i - 1]].proc_freq_hz;
		u32 fh = c->corners[fmax_corner[i]].proc_freq_hz;
		u8 ro = f->ro_sel[i];

		for (j = fmax_corner[i - 1] + 1; j <= fmax_corner[i]; j++)
			c->corners[j].target_quot[ro] = cpr3_interpolate(
				fl, quot_low[i], fh, quot_high[i],
				c->corners[j].proc_freq_hz);
	}

	for (i = 0; i < num_corners; i++) {
		u8 fc = c->corners[i].fuse_corner;
		u8 ro = f->ro_sel[fc];
		s32 qa = cpr3_quot_adjustment(ro_scale[fc], cl_adj_tbl[adj_off + i]);

		c->corners[i].target_quot[ro] += qa;
		if (i > 0 && f->ro_sel[c->corners[i - 1].fuse_corner] == ro &&
		    c->corners[i].target_quot[ro] < c->corners[i - 1].target_quot[ro])
			c->corners[i].target_quot[ro] = c->corners[i - 1].target_quot[ro];
	}

	/* Base target quotients per RO = min nonzero target_quot */
	for (i = 0; i < CPR3_RO_COUNT; i++) {
		u32 mq = UINT_MAX;

		for (j = 0; j < num_corners; j++)
			if (c->corners[j].target_quot[i])
				mq = min(mq, c->corners[j].target_quot[i]);
		c->base_quots[i] = (mq == UINT_MAX) ? 0 : mq;
	}

	/* Append APM and MEM-ACC open-loop crossover corners */
	c->apm_crossover_vc = num_corners;
	c->corners[num_corners] = (struct msm8998_cpr_corner){
		.floor_uv = CPRH_APM_CROSSOVER_UV,
		.ceiling_uv = CPRH_APM_CROSSOVER_UV,
		.open_loop_uv = CPRH_APM_CROSSOVER_UV,
		.use_open_loop = true,
	};
	c->mem_acc_crossover_vc = num_corners + 1;
	c->corners[num_corners + 1] = (struct msm8998_cpr_corner){
		.floor_uv = CPRH_MEM_ACC_CROSSOVER_UV,
		.ceiling_uv = CPRH_MEM_ACC_CROSSOVER_UV,
		.open_loop_uv = CPRH_MEM_ACC_CROSSOVER_UV,
		.use_open_loop = true,
	};
	c->total_corner_count = num_corners + 2;

	/* Resolve OSM LUT open-loop voltages and APM/MEM-ACC thresholds */
	for (i = 0; i < c->lut_len; i++)
		c->opp_ol_mv[i] = c->corners[c->lut[i].virtual_corner].open_loop_uv / 1000U;

	c->apm_threshold_vc = c->func_corner_count - 1;
	c->apm_threshold_pre_vc = c->apm_threshold_vc - 1;
	for (i = 0; i < c->lut_len; i++) {
		if (c->opp_ol_mv[i] * 1000U >= CPRH_APM_THRESHOLD_UV) {
			c->apm_threshold_vc = c->lut[i].virtual_corner;
			c->apm_threshold_pre_vc = c->apm_threshold_vc ?
				c->apm_threshold_vc - 1 : OSM_SEQ_MINUS_ONE;
			break;
		}
	}

	c->mem_acc_threshold_vc = c->func_corner_count - 1;
	c->mem_acc_threshold_pre_vc = c->mem_acc_threshold_vc - 1;
	for (i = 0; i < c->lut_len; i++) {
		if (c->opp_ol_mv[i] * 1000U >= CPRH_MEM_ACC_THRESHOLD_UV) {
			c->mem_acc_threshold_vc = c->lut[i].virtual_corner;
			c->mem_acc_threshold_pre_vc = c->mem_acc_threshold_vc ?
				c->mem_acc_threshold_vc - 1 : OSM_SEQ_MINUS_ONE;
			break;
		}
	}

	return 0;
}

static void log_cluster_summary(struct msm8998_osm_drvdata *d,
				struct msm8998_cluster *c)
{
	u32 i;

	dev_info(d->dev, "%s: speed_bin=%u cpr_rev=%u combo=%u force_hi=%u corners=%u(+2 crossover) lut=%u\n",
		 c->name, c->fuses.speed_bin, c->fuses.cpr_fusing_rev,
		 c->fuses.fuse_combo, c->fuses.force_highest,
		 c->func_corner_count, c->lut_len);
	dev_info(d->dev, "%s: ro_sel=[%u,%u,%u,%u] init_v=[%u,%u,%u,%u] tq=[%u,%u,%u,%u] qo=[%u,%u,%u,%u]\n",
		 c->name,
		 c->fuses.ro_sel[0], c->fuses.ro_sel[1],
		 c->fuses.ro_sel[2], c->fuses.ro_sel[3],
		 c->fuses.init_volt[0], c->fuses.init_volt[1],
		 c->fuses.init_volt[2], c->fuses.init_volt[3],
		 c->fuses.target_quot[0], c->fuses.target_quot[1],
		 c->fuses.target_quot[2], c->fuses.target_quot[3],
		 c->fuses.quot_offset[0], c->fuses.quot_offset[1],
		 c->fuses.quot_offset[2], c->fuses.quot_offset[3]);
	dev_info(d->dev, "%s: apm_thresh_vc=%u(pre=%u) apm_cross_vc=%u mem_acc_thresh_vc=%u(pre=%u) mem_acc_cross_vc=%u\n",
		 c->name, c->apm_threshold_vc, c->apm_threshold_pre_vc,
		 c->apm_crossover_vc, c->mem_acc_threshold_vc,
		 c->mem_acc_threshold_pre_vc, c->mem_acc_crossover_vc);

	for (i = 0; i < c->total_corner_count; i++) {
		const struct msm8998_cpr_corner *cn = &c->corners[i];
		u32 ro = cn->ro_sel;
		u32 dq = cn->proc_freq_hz ?
			DIV_ROUND_UP(cn->target_quot[ro] - c->base_quots[ro],
				     CPRH_DELTA_QUOT_STEP_FACTOR) : 0;

		dev_info(d->dev, "  %s corner[%2u]: freq=%10u flr=%7u ol=%7u ro=%2u quot=%4u dq=%3u%s\n",
			 c->name, i, cn->proc_freq_hz, cn->floor_uv,
			 cn->open_loop_uv, ro, cn->target_quot[ro], dq,
			 cn->use_open_loop ? " (OL)" : "");
	}
}

static inline void cprh_masked_write(void __iomem *base, u32 offset,
				     u32 mask, u32 val)
{
	u32 reg = readl_relaxed(base + offset);

	reg = (reg & ~mask) | (val & mask);
	writel_relaxed(reg, base + offset);
}

static void init_saw4_avs(struct msm8998_cluster *c)
{
	writel_relaxed(c->saw4_avs_limit, c->saw4_base + SAW4_REG_AVS_LIMIT);
	wmb();
	writel_relaxed(SAW4_AVS_CTL_VAL, c->saw4_base + SAW4_REG_AVS_CTL);
	wmb();
}

static int init_cprh_hw(struct msm8998_osm_drvdata *d,
			struct msm8998_cluster *c)
{
	u32 ro_used = 0, ro_mask = CPR3_RO_MASK, gcnt, cont_dly, settle, sw_dly, reg;
	u64 temp;
	u32 i;

	for (i = 0; i < c->total_corner_count; i++) {
		u32 j;

		for (j = 0; j < CPR3_RO_COUNT; j++)
			if (c->corners[i].target_quot[j])
				ro_used |= BIT(j);
		if (c->corners[i].proc_freq_hz)
			ro_mask &= ~BIT(c->corners[i].ro_sel);
	}

	temp = (u64)CPRH_CPR_CLK_RATE_HZ * 1000ULL; /* sensor_time = 1000 ns */
	gcnt = (u32)DIV_ROUND_UP_ULL(temp, 1000000000ULL) - 1;
	for (i = 0; i < CPR3_RO_COUNT; i++)
		if (ro_used & BIT(i))
			writel_relaxed(gcnt, c->cprh_base + CPR3_REG_GCNT(i));

	cont_dly = (u64)CPRH_CPR_CLK_RATE_HZ * 5000000ULL / 1000000000ULL;
	writel_relaxed(cont_dly, c->cprh_base + CPR3_REG_CPR_TIMER_AUTO_CONT);

	/* idle_clocks=15, count_mode=0 (all at once min), count_repeat=1, loop_en=0 */
	reg = (15U << CPR3_CPR_CTL_IDLE_CLOCKS_SHIFT) |
	      (1U << CPR3_CPR_CTL_COUNT_REPEAT_SHIFT);
	writel_relaxed(reg, c->cprh_base + CPR3_REG_CPR_CTL);

	reg = (c->step_quot_min & 0x3F) | ((c->step_quot_max & 0x3F) << 6);
	writel_relaxed(reg, c->cprh_base + CPR3_REG_CPR_STEP_QUOT);

	for (i = 0; i < c->cpr_sensor_count; i++)
		writel_relaxed(0, c->cprh_base + CPR3_REG_SENSOR_OWNER(i));

	/* thread 0 thresholds from stock_02: up=2, dn=2, cons_up=0, cons_dn=2 */
	reg = (2U << 0) | (0U << 4) | (2U << 8) | (2U << 13);
	writel_relaxed(reg, c->cprh_base + CPR3_REG_THRESH(0));
	writel_relaxed(ro_mask, c->cprh_base + CPR3_REG_RO_MASK(0));

	for (i = 0; i < CPR3_RO_COUNT; i++)
		writel_relaxed(c->base_quots[i],
			       c->cprh_base + CPR3_REG_TARGET_QUOT(0, i));

	for (i = 0; i < c->total_corner_count; i++) {
		const struct msm8998_cpr_corner *cn = &c->corners[i];
		u32 ro = cn->proc_freq_hz ? cn->ro_sel : 0;
		u32 ol_steps = DIV_ROUND_UP(cn->open_loop_uv - CPRH_BASE_VOLT_UV,
					    CPRH_STEP_VOLT_UV);
		u32 flr_steps = DIV_ROUND_UP(cn->floor_uv - CPRH_BASE_VOLT_UV,
					     CPRH_STEP_VOLT_UV);
		u32 dq_steps = cn->proc_freq_hz ?
			DIV_ROUND_UP(cn->target_quot[ro] - c->base_quots[ro],
				     CPRH_DELTA_QUOT_STEP_FACTOR) : 0;

		if (ol_steps > 255 || flr_steps > 255 || dq_steps > 511) {
			dev_err(d->dev, "%s: CPRh corner %u out of range (ol=%u flr=%u dq=%u)\n",
				c->name, i, ol_steps, flr_steps, dq_steps);
			return -EINVAL;
		}

		reg = (ol_steps & CPRH_CORNER_INIT_VOLT_MASK) |
		      ((flr_steps << CPRH_CORNER_FLOOR_VOLT_SHIFT) & CPRH_CORNER_FLOOR_VOLT_MASK) |
		      ((dq_steps << CPRH_CORNER_QUOT_DELTA_SHIFT) & CPRH_CORNER_QUOT_DELTA_MASK) |
		      ((ro << CPRH_CORNER_RO_SEL_SHIFT) & CPRH_CORNER_RO_SEL_MASK);
		if (cn->use_open_loop)
			reg |= CPRH_CORNER_CPR_CL_DISABLE;

		writel_relaxed(reg, c->cprh_base + CPRH_REG_CORNER(i));
	}

	/* Dynamic per-RO KV margin + 4 mV PMIC step size */
	cprh_masked_write(c->cprh_base, CPR4_REG_MARGIN_ADJ_CTL,
			  CPR4_MARGIN_ADJ_CTL_HW_CL_EN |
			  CPR4_MARGIN_ADJ_CTL_PER_RO_KV_EN |
			  GENMASK(16, 12) | GENMASK(31, 26),
			  (closed_loop ? CPR4_MARGIN_ADJ_CTL_HW_CL_EN : 0) |
			  CPR4_MARGIN_ADJ_CTL_PER_RO_KV_EN |
			  ((CPRH_STEP_VOLT_UV / 1000U) << CPR4_MARGIN_ADJ_CTL_PMIC_STEP_SHIFT));

	writel_relaxed((1U << 5) | 1U,
		       c->cprh_base + CPR4_REG_SAW_ERROR_STEP_LIMIT);

	settle = ((u64)CPRH_CPR_CLK_RATE_HZ * 1760ULL) / 1000000000ULL;
	cprh_masked_write(c->cprh_base, CPR4_REG_MARGIN_TEMP_CORE_TIMERS,
			  CPR4_MARGIN_TEMP_CORE_TIMERS_SETTLE_MASK,
			  settle << CPR4_MARGIN_TEMP_CORE_TIMERS_SETTLE_SHIFT);

	sw_dly = (((u64)CPRH_CPR_CLK_RATE_HZ * 1042ULL) / 1000000000ULL) /
		 CPRH_MODE_SWITCH_DELAY_FACTOR;
	reg = ((DIV_ROUND_UP(CPRH_BASE_VOLT_UV, CPRH_STEP_VOLT_UV)
		<< CPRH_CTL_BASE_VOLTAGE_SHIFT) & CPRH_CTL_BASE_VOLTAGE_MASK) |
	      (((CPRH_STEP_VOLT_UV / 1000U)
		<< CPRH_CTL_VOLT_MULT_SHIFT) & CPRH_CTL_VOLT_MULT_MASK) |
	      ((sw_dly << CPRH_CTL_MODE_SWITCH_DELAY_SHIFT) & CPRH_CTL_MODE_SWITCH_DELAY_MASK) |
	      CPRH_CTL_OSM_ENABLED;
	cprh_masked_write(c->cprh_base, CPRH_REG_CTL,
			  CPRH_CTL_BASE_VOLTAGE_MASK | CPRH_CTL_VOLT_MULT_MASK |
			  CPRH_CTL_MODE_SWITCH_DELAY_MASK | CPRH_CTL_OSM_ENABLED,
			  reg);

	if (closed_loop)
		cprh_masked_write(c->cprh_base, CPR3_REG_CPR_CTL,
				  CPR3_CPR_CTL_LOOP_EN_MASK,
				  CPR3_CPR_CTL_LOOP_EN_MASK);
	wmb();

	return 0;
}

static u32 osm_count_ns(u64 nsec)
{
	return (u32)div_u64((u64)OSM_CLK_RATE_HZ * nsec, 1000000000ULL);
}

static int acd_wait_status(void __iomem *acd_base, u32 stat_off,
			   u32 mask, u32 timeout_ns)
{
	int t;

	for (t = timeout_ns; t > 0; t -= 100) {
		if (readl_relaxed(acd_base + stat_off) & mask)
			return 0;
		ndelay(100);
	}
	return -ETIMEDOUT;
}

static int acd_master_write_through(struct msm8998_osm_drvdata *d,
				    struct msm8998_cluster *c,
				    u32 val, u32 offset)
{
	u32 ctl = ((offset / 4U) << 1) | BIT(0);
	int rc;

	writel_relaxed(val, c->acd_base + offset);
	readl_relaxed(c->acd_base + ACD_HW_VERSION);
	writel_relaxed(0, c->acd_base + ACD_WRITE_CTL);
	writel_relaxed(ctl, c->acd_base + ACD_WRITE_CTL);
	readl_relaxed(c->acd_base + ACD_HW_VERSION);

	rc = acd_wait_status(c->acd_base, ACD_WRITE_STATUS,
			     ACD_BITMASK(offset), ACD_LOCAL_XFER_TIMEOUT_NS);
	if (rc)
		dev_err(d->dev, "%s: ACD write-through timeout off=0x%x\n",
			c->name, offset);
	return rc;
}

static int init_cluster_acd(struct msm8998_osm_drvdata *d,
			    struct msm8998_cluster *c)
{
	u32 xfer_mask = ACD_BITMASK(ACDTD) | ACD_BITMASK(ACDCR) |
			ACD_BITMASK(ACDSSCR) | ACD_BITMASK(ACD_EXTINT_CFG);
	int rc;

	writel_relaxed(0x9611U, c->acd_base + ACDTD);
	writel_relaxed(0x2B5FFDU, c->acd_base + ACDCR);
	writel_relaxed(0x501U, c->acd_base + ACDSSCR);
	writel_relaxed(0x2CF9AE8U, c->acd_base + ACD_EXTINT_CFG);
	writel_relaxed(0x15U, c->acd_base + ACD_AUTOXFER_CTL);
	readl_relaxed(c->acd_base + ACD_HW_VERSION);

	writel_relaxed(xfer_mask, c->acd_base + ACD_AUTOXFER_CFG);
	writel_relaxed(0, c->acd_base + ACD_AUTOXFER);
	writel_relaxed(BIT(0), c->acd_base + ACD_AUTOXFER);
	readl_relaxed(c->acd_base + ACD_HW_VERSION);

	rc = acd_wait_status(c->acd_base, ACD_AUTOXFER_STATUS,
			     BIT(0), ACD_LOCAL_XFER_TIMEOUT_NS * 4);
	if (rc) {
		dev_err(d->dev, "%s: ACD auto-xfer timeout\n", c->name);
		return rc;
	}

	rc = acd_master_write_through(d, c, BIT(0), ACD_GFMUX_CFG);
	if (rc)
		return rc;
	rc = acd_master_write_through(d, c, BIT(0), ACD_DCVS_SW);
	if (rc)
		return rc;
	rc = acd_master_write_through(d, c, 0, ACD_DCVS_SW);
	if (rc)
		return rc;
	udelay(1);
	rc = acd_master_write_through(d, c, 0x2CF9AFEU, ACD_EXTINT_CFG);
	if (rc)
		return rc;

	xfer_mask |= ACD_BITMASK(ACD_GFMUX_CFG);
	writel_relaxed(xfer_mask, c->acd_base + ACD_AUTOXFER_CFG);
	return 0;
}

static int setup_osm_rcg_200mhz(struct msm8998_osm_drvdata *d)
{
	u32 cfg;
	int t;

	/* HMSS_GPLL0_CLK_SRC (300 MHz, parent 1) / 1.5 (FIXDIV=2) -> 200 MHz */
	cfg = readl_relaxed(d->apcs_common_base + APCS_COMMON_LMH_CFG_RCGR);
	cfg &= ~(GENMASK(10, 8) | GENMASK(4, 0));
	cfg |= (1U << 8) | 2U;
	writel_relaxed(cfg, d->apcs_common_base + APCS_COMMON_LMH_CFG_RCGR);
	wmb();
	writel_relaxed(readl_relaxed(d->apcs_common_base + APCS_COMMON_LMH_CMD_RCGR) | BIT(0),
		       d->apcs_common_base + APCS_COMMON_LMH_CMD_RCGR);
	mb();

	for (t = 500; t > 0; t--) {
		if (!(readl_relaxed(d->apcs_common_base + APCS_COMMON_LMH_CMD_RCGR) & BIT(0)))
			return 0;
		udelay(1);
	}

	dev_err(d->dev, "APCS_COMMON_LMH_CMD_RCGR update timed out\n");
	return -ETIMEDOUT;
}

static void init_cluster_osm_hw(struct msm8998_osm_drvdata *d,
				struct msm8998_cluster *c)
{
	u32 i, val, curr_level, j = 0;
	u32 mem_acc_map[3] = { 0, 0, 0 };
	u32 thresh_vc[4];
	u32 ratio = OSM_CLK_RATE_HZ / OSM_XO_RATE_HZ;
	u32 freq_val = 0, volt_val = 0, ov_val = 0, sp_val = 0;

	/* Cycle counter */
	writel_relaxed(BIT(0) | ((ratio - 1) << 1) | BIT(8),
		       c->osm_base + OSM_CYCLE_COUNTER_CTRL);

	/* Populate 40-entry OSM HW table, repeating final row past lut_len */
	for (i = 0; i < OSM_TABLE_SIZE; i++) {
		u32 off = i * OSM_REG_STRIDE;

		if (i < c->lut_len) {
			freq_val = c->lut[i].freq_data;
			volt_val = ((c->lut[i].virtual_corner & 0x3F) << 16) |
				   (c->opp_ol_mv[i] & 0xFFF);
			ov_val = c->lut[i].override_data;
			sp_val = c->lut[i].spare_data;
		}
		writel_relaxed(i, c->osm_base + OSM_INDEX_REG + off);
		writel_relaxed(freq_val, c->osm_base + OSM_FREQ_REG + off);
		writel_relaxed(volt_val, c->osm_base + OSM_VOLT_REG + off);
		writel_relaxed(ov_val, c->osm_base + OSM_OVERRIDE_REG + off);
		writel_relaxed(sp_val, c->osm_base + OSM_SPARE_REG + off);
	}
	readl_relaxed(c->osm_base + OSM_VERSION_REG);

	/* CC policy: up/down 1000us (200 cycles @ 200MHz), pc-override 0+ENABLE, ret=active, cc_dcvs=on */
	val = (osm_count_ns(1000) << 16) | osm_count_ns(1000);
	writel_relaxed(val, c->osm_base + OSM_SPM_CC_HYSTERESIS);
	writel_relaxed(BIT(0), c->osm_base + OSM_CC_ZERO_BEHAV_CTRL);
	writel_relaxed(0, c->osm_base + OSM_SPM_CORE_RET_MAPPING);
	writel_relaxed(0, c->osm_base + OSM_SPM_CC_DCVS_DISABLE);

	/* LLM freq & volt policy (327675 ns up/down, both honored) */
	val = (osm_count_ns(327675) << 16) | osm_count_ns(327675);
	writel_relaxed(val, c->osm_base + OSM_LLM_FREQ_VOTE_HYST);
	writel_relaxed(val, c->osm_base + OSM_LLM_VOLT_VOTE_HYST);
	writel_relaxed(0, c->osm_base + OSM_LLM_INTF_DCVS_DISABLE);

	/* FSMs: boost, safe, ps, wfx, pc, droop */
	writel_relaxed(readl_relaxed(c->osm_base + OSM_PDN_FSM_CTRL_REG) | 0x3FU,
		       c->osm_base + OSM_PDN_FSM_CTRL_REG);

	val = osm_count_ns(10000) | (osm_count_ns(5000) << 16);
	writel_relaxed(val, c->osm_base + OSM_CC_BOOST_TIMER_REG0);
	writel_relaxed(val, c->osm_base + OSM_DCVS_BOOST_TIMER_REG0);
	writel_relaxed(val, c->osm_base + OSM_PS_BOOST_TIMER_REG0);

	val = osm_count_ns(10000) | (osm_count_ns(10000) << 16);
	writel_relaxed(val, c->osm_base + OSM_CC_BOOST_TIMER_REG1);
	writel_relaxed(val, c->osm_base + OSM_DCVS_BOOST_TIMER_REG1);
	writel_relaxed(val, c->osm_base + OSM_PS_BOOST_TIMER_REG1);

	val = osm_count_ns(1000);
	writel_relaxed(val, c->osm_base + OSM_CC_BOOST_TIMER_REG2);
	writel_relaxed(val, c->osm_base + OSM_DCVS_BOOST_TIMER_REG2);
	writel_relaxed(val, c->osm_base + OSM_PS_BOOST_TIMER_REG2);
	writel_relaxed(0x5U, c->osm_base + OSM_BOOST_PROG_SYNC_DELAY_REG);

	writel_relaxed((osm_count_ns(500) << 16) | osm_count_ns(500),
		       c->osm_base + OSM_DROOP_UNSTALL_TIMER_CTRL);
	writel_relaxed((osm_count_ns(250) << 16) | osm_count_ns(250),
		       c->osm_base + OSM_DROOP_WAIT_TO_REL_CTRL0);
	writel_relaxed(0x1U, c->osm_base + OSM_DROOP_PROG_SYNC_DELAY_REG);
	writel_relaxed(osm_count_ns(5), c->osm_base + OSM_DROOP_RELEASE_TIMER_CTRL);
	writel_relaxed(osm_count_ns(500), c->osm_base + OSM_DCVS_DROOP_TIMER_CTRL);
	writel_relaxed(readl_relaxed(c->osm_base + OSM_DROOP_CTRL_REG) |
		       BIT(31) | (0x2U << 16) | 0x8U,
		       c->osm_base + OSM_DROOP_CTRL_REG);
	writel_relaxed(readl_relaxed(c->osm_base + OSM_PLL_SW_OVERRIDE_EN) | BIT(0),
		       c->osm_base + OSM_PLL_SW_OVERRIDE_EN);

	/* MEM-ACC SCM writes (!secure_init path) */
	curr_level = c->lut[0].spare_data;
	for (i = 0; i < c->lut_len && j < 2; i++) {
		if (c->lut[i].spare_data != curr_level) {
			mem_acc_map[j++] = c->lut[i].virtual_corner - 1;
			curr_level = c->lut[i].spare_data;
		}
	}
	qcom_scm_io_writel(c->osm_phys + OSM_SEQ_REG(88), c->mem_acc_crossover_vc);

	thresh_vc[0] = mem_acc_map[0];
	thresh_vc[1] = mem_acc_map[0] + 1;
	thresh_vc[2] = c->mem_acc_threshold_pre_vc;
	thresh_vc[3] = c->mem_acc_threshold_vc;
	if (c->mem_acc_threshold_pre_vc == OSM_SEQ_MINUS_ONE) {
		thresh_vc[0] = thresh_vc[1] = OSM_SEQ_MINUS_ONE;
	} else {
		if (thresh_vc[1] >= thresh_vc[2])
			thresh_vc[1] = thresh_vc[2] - 1;
		if (thresh_vc[0] >= thresh_vc[1])
			thresh_vc[0] = thresh_vc[1] - 1;
	}
	qcom_scm_io_writel(c->osm_phys + OSM_SEQ_REG(55), thresh_vc[0]);
	qcom_scm_io_writel(c->osm_phys + OSM_SEQ_REG(56), thresh_vc[1]);
	qcom_scm_io_writel(c->osm_phys + OSM_SEQ_REG(57), thresh_vc[2]);
	qcom_scm_io_writel(c->osm_phys + OSM_SEQ_REG(58), thresh_vc[3]);

	for (i = 0; i < c->lut_len; i++) {
		if (c->lut[i].virtual_corner == c->mem_acc_threshold_vc)
			qcom_scm_io_writel(c->osm_phys + OSM_SEQ_REG(32),
					   c->lut[i].freq_data & 0xFFU);
	}

	/* APM VC setup for MSM8998 v2 */
	writel_relaxed(c->apm_threshold_vc, c->osm_base + OSM_SEQ_REG1_V2);
	qcom_scm_io_writel(c->osm_phys + OSM_SEQ_REG(72), c->apm_crossover_vc);
	qcom_scm_io_writel(c->osm_phys + OSM_SEQ_REG(15), c->apm_threshold_vc);
	qcom_scm_io_writel(c->osm_phys + OSM_SEQ_REG(31), c->apm_threshold_pre_vc);
	qcom_scm_io_writel(c->osm_phys + OSM_SEQ_REG(76),
			   0x39U | (c->apm_threshold_vc << 6));
	readl_relaxed(c->osm_base + OSM_VERSION_REG);
}

static int find_osm_lut_idx(const struct msm8998_cluster *c, u32 freq_hz)
{
	int quad_idx = -1, single_idx = -1;
	u32 i;

	for (i = 0; i < c->lut_len; i++) {
		u32 cc = (c->lut[i].freq_data & GENMASK(18, 16)) >> 16;

		if (c->lut[i].freq_hz == freq_hz) {
			if (cc == OSM_MAX_CORE_COUNT)
				quad_idx = i;
			else if (cc == 1)
				single_idx = i;
		}
	}

	return (quad_idx >= 0) ? quad_idx : single_idx;
}

static int msm8998_osm_target_index(struct cpufreq_policy *policy,
				    unsigned int index)
{
	struct msm8998_cluster *c = policy->driver_data;
	u32 lut_idx = policy->freq_table[index].driver_data;

	writel_relaxed(lut_idx, c->osm_base + OSM_DCVS_PERF_STATE_DESIRED);
	readl_relaxed(c->osm_base + OSM_VERSION_REG);
	return 0;
}

static unsigned int msm8998_osm_fast_switch(struct cpufreq_policy *policy,
					    unsigned int target_freq)
{
	struct msm8998_cluster *c = policy->driver_data;
	unsigned int index = policy->cached_resolved_idx;
	u32 lut_idx = policy->freq_table[index].driver_data;

	writel_relaxed(lut_idx, c->osm_base + OSM_DCVS_PERF_STATE_DESIRED);
	return policy->freq_table[index].frequency;
}

static unsigned int msm8998_osm_get(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);
	struct msm8998_cluster *c;
	u32 lut_idx;

	if (!policy)
		return 0;

	c = policy->driver_data;
	lut_idx = readl_relaxed(c->osm_base + OSM_DCVS_PERF_STATE_DESIRED);
	if (lut_idx >= c->lut_len)
		return 0;

	return c->lut[lut_idx].freq_hz / 1000U;
}

static int msm8998_osm_cpu_init(struct cpufreq_policy *policy)
{
	struct msm8998_cluster *c = NULL;
	struct device *cpu_dev = get_cpu_device(policy->cpu);
	struct cpufreq_frequency_table *table;
	u32 i, k = 0, prev_khz = 0;
	int rc;

	if (!cpu_dev || !g_osm_data)
		return -ENODEV;

	for (i = 0; i < MSM8998_NUM_CLUSTERS; i++) {
		if (cpumask_test_cpu(policy->cpu, &g_osm_data->cl[i].cpus)) {
			c = &g_osm_data->cl[i];
			break;
		}
	}
	if (!c)
		return -ENODEV;

	table = kcalloc(c->lut_len + 1, sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;

	for (i = 0; i < c->lut_len; i++) {
		u32 cc = (c->lut[i].freq_data & GENMASK(18, 16)) >> 16;
		u32 freq_khz = c->lut[i].freq_hz / 1000U;
		bool last = (i == c->lut_len - 1);

		if (cc != OSM_MAX_CORE_COUNT && !last)
			continue;
		if (freq_khz == prev_khz)
			continue;

		rc = dev_pm_opp_add(cpu_dev, c->lut[i].freq_hz,
				    c->opp_ol_mv[i] * 1000UL);
		if (rc) {
			dev_err(cpu_dev, "failed to add OPP %u kHz: %d\n",
				freq_khz, rc);
			dev_pm_opp_remove_all_dynamic(cpu_dev);
			kfree(table);
			return rc;
		}

		table[k].frequency = freq_khz;
		table[k].driver_data = i;
		prev_khz = freq_khz;
		k++;
	}
	table[k].frequency = CPUFREQ_TABLE_END;

	policy->driver_data = c;
	policy->freq_table = table;
	cpumask_copy(policy->cpus, &c->cpus);
	dev_pm_opp_set_sharing_cpus(cpu_dev, policy->cpus);
	policy->cpuinfo.transition_latency = 10000;
	policy->fast_switch_possible = true;

	dev_info(g_osm_data->dev, "%s policy cpu%u (%u OPPs: %u..%u kHz, drv=%p)\n",
		 c->name, policy->cpu, k, table[0].frequency,
		 table[k - 1].frequency, policy->driver_data);
	return 0;
}

static void msm8998_osm_cpu_exit(struct cpufreq_policy *policy)
{
	struct device *cpu_dev = get_cpu_device(policy->cpu);

	if (cpu_dev)
		dev_pm_opp_remove_all_dynamic(cpu_dev);
	kfree(policy->freq_table);
}

static struct cpufreq_driver msm8998_osm_cpufreq_driver = {
	.flags		= CPUFREQ_NEED_INITIAL_FREQ_CHECK |
			  CPUFREQ_HAVE_GOVERNOR_PER_POLICY |
			  CPUFREQ_IS_COOLING_DEV,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= msm8998_osm_target_index,
	.get		= msm8998_osm_get,
	.init		= msm8998_osm_cpu_init,
	.exit		= msm8998_osm_cpu_exit,
	.register_em	= cpufreq_register_em_with_opp,
	.fast_switch	= msm8998_osm_fast_switch,
	.name		= "msm8998-osm",
};

static int map_nonexcl_byname(struct platform_device *pdev, const char *name,
			      void __iomem **vbase, phys_addr_t *pbase)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!res)
		return dev_err_probe(&pdev->dev, -ENODEV, "missing reg %s\n", name);

	*vbase = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!*vbase)
		return -ENOMEM;

	if (pbase)
		*pbase = res->start;
	return 0;
}

static int msm8998_osm_cpufreq_probe(struct platform_device *pdev)
{
	struct msm8998_osm_drvdata *d;
	void __iomem *osm_base;
	phys_addr_t osm_phys;
	u32 cpu;
	int i, rc;
	bool already_enabled;

	if (!probe_only && !qcom_scm_is_available())
		return -EPROBE_DEFER;

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = &pdev->dev;

	rc = map_nonexcl_byname(pdev, "qfprom", &d->qfprom_base, NULL);
	if (rc)
		return rc;

	d->cl[0].id = MSM8998_CLUSTER_PWR;
	d->cl[0].name = "pwrcl";
	d->cl[0].saw4_avs_limit = 0x04200420U;
	d->cl[0].cpr_sensor_count = 6;
	d->cl[0].step_quot_min = 11;
	d->cl[0].step_quot_max = 12;
	d->cl[0].boot_rate_hz = 1555200000U;

	d->cl[1].id = MSM8998_CLUSTER_PERF;
	d->cl[1].name = "perfcl";
	d->cl[1].saw4_avs_limit = 0x04700470U;
	d->cl[1].cpr_sensor_count = 9;
	d->cl[1].step_quot_min = 9;
	d->cl[1].step_quot_max = 14;
	d->cl[1].boot_rate_hz = 1728000000U;

	for_each_present_cpu(cpu) {
		int cid = topology_cluster_id(cpu);

		if (cid >= 0 && cid < MSM8998_NUM_CLUSTERS)
			cpumask_set_cpu(cpu, &d->cl[cid].cpus);
	}

	for (i = 0; i < MSM8998_NUM_CLUSTERS; i++) {
		rc = compute_cluster_cprh(d, &d->cl[i]);
		if (rc)
			return rc;
		log_cluster_summary(d, &d->cl[i]);
	}

	if (probe_only) {
		dev_info(&pdev->dev, "probe_only=1: skipping HW initialization and cpufreq registration\n");
		return 0;
	}

	rc = map_nonexcl_byname(pdev, "osm", &osm_base, &osm_phys);
	if (rc)
		return rc;
	rc = map_nonexcl_byname(pdev, "apcs-common", &d->apcs_common_base, NULL);
	if (rc)
		return rc;
	rc = map_nonexcl_byname(pdev, "pwrcl-acd", &d->cl[0].acd_base, NULL);
	if (rc)
		return rc;
	rc = map_nonexcl_byname(pdev, "perfcl-acd", &d->cl[1].acd_base, NULL);
	if (rc)
		return rc;
	rc = map_nonexcl_byname(pdev, "cprh-pwrcl", &d->cl[0].cprh_base, NULL);
	if (rc)
		return rc;
	rc = map_nonexcl_byname(pdev, "cprh-perfcl", &d->cl[1].cprh_base, NULL);
	if (rc)
		return rc;
	rc = map_nonexcl_byname(pdev, "saw4-pwrcl", &d->cl[0].saw4_base, NULL);
	if (rc)
		return rc;
	rc = map_nonexcl_byname(pdev, "saw4-perfcl", &d->cl[1].saw4_base, NULL);
	if (rc)
		return rc;

	d->cl[0].osm_base = osm_base;
	d->cl[0].osm_phys = osm_phys;
	d->cl[1].osm_base = osm_base + OSM_BANK_STRIDE;
	d->cl[1].osm_phys = osm_phys + OSM_BANK_STRIDE;

	d->aux_clk = devm_clk_get(&pdev->dev, "aux");
	if (IS_ERR(d->aux_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(d->aux_clk), "missing aux clk\n");

	d->rbcpr_clk = devm_clk_get(&pdev->dev, "rbcpr");
	if (IS_ERR(d->rbcpr_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(d->rbcpr_clk), "missing rbcpr clk\n");

	rc = clk_set_rate(d->rbcpr_clk, CPRH_CPR_CLK_RATE_HZ);
	if (rc)
		return dev_err_probe(&pdev->dev, rc, "failed rbcpr rate\n");
	rc = clk_prepare_enable(d->rbcpr_clk);
	if (rc)
		return dev_err_probe(&pdev->dev, rc, "failed rbcpr enable\n");

	already_enabled = (readl_relaxed(d->cl[0].osm_base + OSM_ENABLE_REG) & BIT(0)) &&
			  (readl_relaxed(d->cl[1].osm_base + OSM_ENABLE_REG) & BIT(0));

	if (already_enabled) {
		dev_info(&pdev->dev, "OSM already enabled; updating CPRh closed_loop=%d only\n",
			 closed_loop);
		for (i = 0; i < MSM8998_NUM_CLUSTERS; i++) {
			cprh_masked_write(d->cl[i].cprh_base, CPR3_REG_CPR_CTL,
					  CPR3_CPR_CTL_LOOP_EN_MASK, 0);
			wmb();
			cprh_masked_write(d->cl[i].cprh_base, CPR4_REG_MARGIN_ADJ_CTL,
					  CPR4_MARGIN_ADJ_CTL_HW_CL_EN,
					  closed_loop ? CPR4_MARGIN_ADJ_CTL_HW_CL_EN : 0);
			wmb();
			if (closed_loop)
				cprh_masked_write(d->cl[i].cprh_base, CPR3_REG_CPR_CTL,
						  CPR3_CPR_CTL_LOOP_EN_MASK,
						  CPR3_CPR_CTL_LOOP_EN_MASK);
		}
		goto register_cpufreq;
	}

	/* 1. Configure SAW4 AVS limits/control */
	for (i = 0; i < MSM8998_NUM_CLUSTERS; i++)
		init_saw4_avs(&d->cl[i]);

	/* 2. Program CPRh corners, quotients, timers, and closed/open-loop mode */
	for (i = 0; i < MSM8998_NUM_CLUSTERS; i++) {
		rc = init_cprh_hw(d, &d->cl[i]);
		if (rc)
			goto err_rbcpr_clk;
	}

	/* 3. Program OSM v2 hardware tables, policies, FSMs, MEM-ACC & APM VCs */
	for (i = 0; i < MSM8998_NUM_CLUSTERS; i++)
		init_cluster_osm_hw(d, &d->cl[i]);

	/* 4. Set aux_clk=300MHz, apcs_common LMh RCG=200MHz, select index 0, init ACD & enable OSM */
	rc = clk_set_rate(d->aux_clk, OSM_AUX_RATE_HZ);
	if (rc) {
		dev_err(&pdev->dev, "failed to set aux_clk to 300MHz: %d\n", rc);
		goto err_rbcpr_clk;
	}
	rc = clk_prepare_enable(d->aux_clk);
	if (rc) {
		dev_err(&pdev->dev, "failed to enable aux_clk: %d\n", rc);
		goto err_rbcpr_clk;
	}
	d->clks_enabled = true;

	rc = setup_osm_rcg_200mhz(d);
	if (rc)
		goto err_aux_clk;

	for (i = 0; i < MSM8998_NUM_CLUSTERS; i++) {
		writel_relaxed(0, d->cl[i].osm_base + OSM_DCVS_PERF_STATE_DESIRED);
		readl_relaxed(d->cl[i].osm_base + OSM_VERSION_REG);

		rc = init_cluster_acd(d, &d->cl[i]);
		if (rc)
			goto err_aux_clk;

		udelay(5);
		writel_relaxed(1, d->cl[i].osm_base + OSM_ENABLE_REG);
		readl_relaxed(d->cl[i].osm_base + OSM_VERSION_REG);
		udelay(5);
	}

	for (i = 0; i < MSM8998_NUM_CLUSTERS; i++) {
		int idx = find_osm_lut_idx(&d->cl[i], d->cl[i].boot_rate_hz);

		if (idx >= 0) {
			writel_relaxed(idx, d->cl[i].osm_base + OSM_DCVS_PERF_STATE_DESIRED);
			readl_relaxed(d->cl[i].osm_base + OSM_VERSION_REG);
		}
	}

register_cpufreq:
	g_osm_data = d;
	platform_set_drvdata(pdev, d);

	rc = cpufreq_register_driver(&msm8998_osm_cpufreq_driver);
	if (rc) {
		dev_err(&pdev->dev, "cpufreq_register_driver failed: %d\n", rc);
		g_osm_data = NULL;
		return rc;
	}

	dev_info(&pdev->dev, "MSM8998 OSM v2 + CPRh (%s) + SAW4 initialized cleanly\n",
		 closed_loop ? "HW closed-loop" : "open-loop");
	return 0;

err_aux_clk:
	clk_disable_unprepare(d->aux_clk);
err_rbcpr_clk:
	clk_disable_unprepare(d->rbcpr_clk);
	return rc;
}

static void msm8998_osm_cpufreq_remove(struct platform_device *pdev)
{
	if (g_osm_data) {
		cpufreq_unregister_driver(&msm8998_osm_cpufreq_driver);
		g_osm_data = NULL;
	}
	/* Keep OSM/CPRh clocks active once hardware FSMs own CPU domains */
}

static const struct of_device_id msm8998_osm_cpufreq_match[] = {
	{ .compatible = "qcom,msm8998-osm-cpufreq" },
	{}
};
MODULE_DEVICE_TABLE(of, msm8998_osm_cpufreq_match);

static struct platform_driver msm8998_osm_cpufreq_plat_driver = {
	.probe = msm8998_osm_cpufreq_probe,
	.remove = msm8998_osm_cpufreq_remove,
	.driver = {
		.name = "msm8998-osm-cpufreq",
		.of_match_table = msm8998_osm_cpufreq_match,
	},
};
module_platform_driver(msm8998_osm_cpufreq_plat_driver);

MODULE_DESCRIPTION("Qualcomm MSM8998 OSM v2 + CPRh + SAW4 CPUFreq driver");
MODULE_LICENSE("GPL");
