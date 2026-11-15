// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clean-room WCD9335 Codec Processing Engine (CPE) Xtensa DSP loader.
 *
 * Parses split Qualcomm MDT/ELF32 Xtensa firmware images (cpe_9335.mdt +
 * cpe_9335.bXX), configures the WCD9335 CPE FLL/clock domains, downloads
 * IRAM/DRAM segments over SLIMbus regmap, boots the Xtensa CPAR core, and
 * verifies the CMI system boot + shared-memory allocation handshake.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/elf.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "wcd9335.h"

#define WCD9335_CPE_DEFAULT_FW		"cpe_9335"
#define WCD9335_CPE_FLL_75MHZ		75000000U
#define WCD9335_CPE_MCLK_9P6MHZ		9600000U
#define WCD9335_CPE_MCLK_12P288MHZ	12288000U

#define WCD9335_CPE_DRAM_BASE		0x48000U
#define WCD9335_CPE_DRAM_SIZE		0x34000U
#define WCD9335_CPE_IRAM_BASE		0x80000U
#define WCD9335_CPE_IRAM_SIZE		0x20000U

#define WCD9335_CPE_MEM_IRAM		0x01
#define WCD9335_CPE_MEM_DRAM		0x02
#define WCD9335_CPE_MEM_CTRL_WRITE	0x10
#define WCD9335_CPE_MEM_CTRL_AUTOINC	0x08

#define WCD9335_CPE_MBOX_SIZE		16
#define WCD9335_CPE_CHUNK_SIZE		16
#define WCD9335_CPE_MSG_BUF_SIZE	132

#define CMI_CPE_CORE_SERVICE_ID		1
#define CMI_SVC_ID_SHIFT		4
#define CMI_CORE_CMD_SHARED_MEM_ALLOC	0x3001
#define CMI_CORE_RSP_SHARED_MEM_ALLOC	0x3002
#define CMI_CORE_EVENT_SYSTEM_BOOT	0x3005

struct wcd9335_cmi_hdr {
	u8 hdr_info;
	u8 pld_info;
	__le16 opcode;
} __packed;

struct wcd9335_cmi_boot_event {
	u8 status;
	u8 version;
	__le16 sfr_buff_size;
	__le32 sfr_buff_address;
} __packed;

struct wcd9335_cmi_shmem_req {
	__le32 size;
} __packed;

struct wcd9335_cmi_shmem_rsp {
	__le32 addr;
} __packed;

struct wcd9335_cpe {
	struct device *dev;
	struct regmap *regmap;
	int (*mclk_ctrl)(struct device *dev, bool enable, u32 *mclk_rate);
	int virq_outbox1;
	int virq_err;

	struct mutex lock;
	struct completion outbox_compl;
	bool running;
	bool mclk_held;

	char fw_name[32];
	u32 mclk_rate;
	u8 boot_status;
	u8 cmi_version;
	u16 sfr_size;
	u32 sfr_addr;
	u32 shmem_addr;
	u32 err_count;
	u32 last_ss_status;
};

static int wcd9335_cpe_read_outbox(struct wcd9335_cpe *cpe, u8 *buf)
{
	int i, ret;
	unsigned int val;

	for (i = 0; i < WCD9335_CPE_MBOX_SIZE; i++) {
		ret = regmap_read(cpe->regmap, WCD9335_CPE_SS_OUTBOX1_0 + i, &val);
		if (ret)
			return ret;
		buf[i] = val & 0xff;
	}

	return regmap_write(cpe->regmap, WCD9335_CPE_SS_OUTBOX1_ACK, 0x01);
}

static irqreturn_t wcd9335_cpe_outbox_irq(int irq, void *data)
{
	struct wcd9335_cpe *cpe = data;

	complete(&cpe->outbox_compl);
	return IRQ_HANDLED;
}

static irqreturn_t wcd9335_cpe_err_irq(int irq, void *data)
{
	struct wcd9335_cpe *cpe = data;
	unsigned int st = 0;

	regmap_read(cpe->regmap, WCD9335_CPE_SS_SS_ERROR_INT_STATUS, &st);
	regmap_write(cpe->regmap, WCD9335_CPE_SS_SS_ERROR_INT_CLEAR, st);
	cpe->err_count++;
	dev_err(cpe->dev, "CPE error IRQ triggered (ss_err=0x%02x)\n", st);
	return IRQ_HANDLED;
}

static int wcd9335_cpe_enable_rco(struct wcd9335_cpe *cpe, bool enable)
{
	struct regmap *rm = cpe->regmap;
	unsigned int rco_val = 0;

	if (!enable) {
		regmap_update_bits(rm, WCD9335_ANA_CLK_TOP, 0x08, 0x00);
		regmap_update_bits(rm, WCD9335_ANA_RCO, 0x40, 0x00);
		return 0;
	}

	/* Enable BG + internal RCO and calibrate against active MCLK */
	regmap_update_bits(rm, WCD9335_ANA_RCO, 0x80, 0x80);
	usleep_range(20, 30);
	regmap_update_bits(rm, WCD9335_ANA_RCO, 0x40, 0x40);
	usleep_range(20, 30);
	regmap_update_bits(rm, WCD9335_ANA_RCO, 0x04, 0x04);
	regmap_update_bits(rm, WCD9335_ANA_RCO, 0x04, 0x00);
	usleep_range(5000, 6000);

	regmap_read(rm, WCD9335_ANA_RCO, &rco_val);
	if (rco_val & 0x02)
		dev_warn(cpe->dev, "WCD9335 CPE RCO calibration incomplete (0x%02x)\n",
			 rco_val);

	regmap_update_bits(rm, WCD9335_ANA_CLK_TOP, 0x08, 0x08);
	return 0;
}

static int wcd9335_cpe_set_fll_clk(struct wcd9335_cpe *cpe, bool enable)
{
	struct regmap *rm = cpe->regmap;
	u32 mclk_rate = cpe->mclk_rate;
	u32 div_val, l_val, computed;
	int ret = 0;

	if (!enable) {
		regmap_update_bits(rm, WCD9335_ANA_CLK_TOP, 0x01, 0x00);
		regmap_update_bits(rm, WCD9335_CPE_FLL_TEST_CTL_0, 0x80, 0x00);
		regmap_write(rm, WCD9335_CPE_FLL_FLL_MODE, 0x00);
		regmap_update_bits(rm, WCD9335_CPE_FLL_FLL_MODE, 0x60, 0x20);
		regmap_update_bits(rm, WCD9335_CPE_FLL_USER_CTL_0, 0x01, 0x00);
		wcd9335_cpe_enable_rco(cpe, false);
		return 0;
	}

	ret = wcd9335_cpe_enable_rco(cpe, true);
	if (ret)
		return ret;

	ret |= regmap_update_bits(rm, WCD9335_CPE_FLL_FLL_MODE, 0x08, 0x08);
	if (mclk_rate == WCD9335_CPE_MCLK_12P288MHZ) {
		ret |= regmap_write(rm, WCD9335_CPE_FLL_USER_CTL_1, 0x55);
		div_val = 10;
	} else if (mclk_rate == WCD9335_CPE_MCLK_9P6MHZ) {
		ret |= regmap_update_bits(rm, WCD9335_CPE_FLL_USER_CTL_0, 0x7c, 0x70);
		ret |= regmap_update_bits(rm, WCD9335_CPE_FLL_USER_CTL_1, 0xe0, 0x20);
		div_val = 8;
	} else {
		regmap_update_bits(rm, WCD9335_CPE_FLL_FLL_MODE, 0x08, 0x00);
		dev_err(cpe->dev, "Unsupported MCLK rate %u for CPE FLL\n", mclk_rate);
		return -EINVAL;
	}

	l_val = ((WCD9335_CPE_FLL_75MHZ / 1000) * div_val) / (mclk_rate / 1000);
	computed = (mclk_rate / div_val) * l_val;
	if (computed < WCD9335_CPE_FLL_75MHZ)
		l_val++;

	ret |= regmap_write(rm, WCD9335_CPE_FLL_L_VAL_CTL_0, l_val & 0xff);
	ret |= regmap_write(rm, WCD9335_CPE_FLL_L_VAL_CTL_1, (l_val >> 8) & 0xff);
	ret |= regmap_update_bits(rm, WCD9335_CPE_FLL_FLL_MODE, 0x10, 0x10);
	ret |= regmap_update_bits(rm, WCD9335_CPE_FLL_FLL_MODE, 0x10, 0x00);
	ret |= regmap_update_bits(rm, WCD9335_CPE_FLL_FLL_MODE, 0x08, 0x00);

	/* Tasha v2.0 uses calibrated internal RCO (ANA_CLK_TOP bit 1 = 0) */
	ret |= regmap_update_bits(rm, WCD9335_ANA_CLK_TOP, 0x02, 0x00);
	ret |= regmap_update_bits(rm, WCD9335_ANA_CLK_TOP, 0x01, 0x01);
	ret |= regmap_update_bits(rm, WCD9335_CPE_FLL_USER_CTL_0, 0x01, 0x01);
	ret |= regmap_update_bits(rm, WCD9335_CPE_FLL_TEST_CTL_0, 0x80, 0x80);
	ret |= regmap_update_bits(rm, WCD9335_CPE_FLL_FLL_MODE, 0x60, 0x00);
	ret |= regmap_write(rm, WCD9335_CPE_FLL_FLL_MODE, 0x80);
	if (!ret) {
		unsigned int fll_st3 = 0;
		int tries;

		for (tries = 0; tries < 50; tries++) {
			usleep_range(1000, 1200);
			if (!regmap_read(rm, WCD9335_CPE_FLL_STATUS_3, &fll_st3) &&
			    (fll_st3 & 0x01))
				break;
		}
		if (!(fll_st3 & 0x01))
			dev_dbg(cpe->dev, "CPE FLL status3=0x%02x after lock wait\n",
				 fll_st3);
	}
	return ret;
}

static int wcd9335_cpe_reset_hw(struct wcd9335_cpe *cpe)
{
	struct regmap *rm = cpe->regmap;
	int ret = 0;

	ret |= regmap_write(rm, WCD9335_CPE_SS_CPAR_CFG, 0x00);
	ret |= regmap_write(rm, WCD9335_CODEC_RPM_PWR_CPE_IRAM_SHUTDOWN, 0x00);
	ret |= regmap_write(rm, WCD9335_CODEC_RPM_PWR_CPE_DRAM1_SHUTDOWN, 0x00);
	ret |= regmap_write(rm, WCD9335_CODEC_RPM_PWR_CPE_DRAM0_SHUTDOWN_1, 0x00);
	ret |= regmap_write(rm, WCD9335_CODEC_RPM_PWR_CPE_DRAM0_SHUTDOWN_2, 0x00);
	ret |= regmap_update_bits(rm, WCD9335_CPE_SS_CPAR_CTL, 0x04, 0x04);
	return ret;
}

static int wcd9335_cpe_set_mem_ptr(struct wcd9335_cpe *cpe, u32 offset, u8 ctrl)
{
	struct regmap *rm = cpe->regmap;
	int ret = 0;

	ret |= regmap_write(rm, WCD9335_CPE_SS_MEM_CTRL, ctrl & 0x1f);
	ret |= regmap_write(rm, WCD9335_CPE_SS_MEM_PTR_0, offset & 0xff);
	ret |= regmap_write(rm, WCD9335_CPE_SS_MEM_PTR_1, (offset >> 8) & 0xff);
	ret |= regmap_write(rm, WCD9335_CPE_SS_MEM_PTR_2, (offset >> 16) & 0xff);
	return ret;
}

static int wcd9335_cpe_read_ram(struct wcd9335_cpe *cpe, u8 mem_type,
				u32 offset, u8 *buf, size_t len)
{
	struct regmap *rm = cpe->regmap;
	u8 ctrl = WCD9335_CPE_MEM_CTRL_WRITE | WCD9335_CPE_MEM_CTRL_AUTOINC | mem_type;
	unsigned int val;
	size_t i;
	int ret;

	ret = regmap_write(rm, WCD9335_CPE_SS_MEM_CTRL, 0);
	if (ret)
		return ret;

	ret = regmap_write(rm, WCD9335_CPE_SS_MEM_PTR_0, offset & 0xff);
	ret |= regmap_write(rm, WCD9335_CPE_SS_MEM_PTR_1, (offset >> 8) & 0xff);
	ret |= regmap_write(rm, WCD9335_CPE_SS_MEM_PTR_2, (offset >> 16) & 0xff);
	ret |= regmap_write(rm, WCD9335_CPE_SS_MEM_CTRL, ctrl);
	if (ret)
		return ret;

	for (i = 0; i < len; i++) {
		ret = regmap_read(rm, WCD9335_CPE_SS_MEM_BANK_0, &val);
		if (ret)
			break;
		buf[i] = val & 0xff;
	}

	regmap_write(rm, WCD9335_CPE_SS_MEM_CTRL, 0);
	return ret;
}

static int wcd9335_cpe_write_bytes(struct wcd9335_cpe *cpe, const u8 *data, size_t size)
{
	struct regmap *rm = cpe->regmap;
	size_t i;
	int ret;

	for (i = 0; i < size; i++) {
		ret = regmap_write(rm, WCD9335_CPE_SS_MEM_BANK_0, data[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int wcd9335_cpe_write_segment(struct wcd9335_cpe *cpe, u32 paddr, u32 flags,
				     const u8 *data, size_t size)
{
	struct regmap *rm = cpe->regmap;
	u8 mem_type, ctrl;
	u32 base, max_sz, offset;
	int ret;

	if (flags & PF_X) {
		mem_type = WCD9335_CPE_MEM_IRAM;
		base = WCD9335_CPE_IRAM_BASE;
		max_sz = WCD9335_CPE_IRAM_SIZE;
	} else if (flags & (PF_R | PF_W)) {
		mem_type = WCD9335_CPE_MEM_DRAM;
		base = WCD9335_CPE_DRAM_BASE;
		max_sz = WCD9335_CPE_DRAM_SIZE;
	} else {
		dev_err(cpe->dev, "Invalid segment flags 0x%x\n", flags);
		return -EINVAL;
	}

	if (paddr < base || (paddr - base) + size > max_sz) {
		dev_err(cpe->dev, "CPE segment out of range: paddr=0x%x size=0x%zx\n",
			paddr, size);
		return -EINVAL;
	}

	offset = paddr - base;

	ctrl = WCD9335_CPE_MEM_CTRL_AUTOINC | mem_type;
	ret = wcd9335_cpe_set_mem_ptr(cpe, offset, ctrl);
	if (ret)
		return ret;

	ret = wcd9335_cpe_write_bytes(cpe, data, size);

	regmap_write(rm, WCD9335_CPE_SS_MEM_CTRL, 0);
	if (!ret && size >= 4) {
		u8 verify[16];
		size_t vlen = min_t(size_t, sizeof(verify), size);

		if (!wcd9335_cpe_read_ram(cpe, mem_type, offset, verify, vlen) &&
		    memcmp(data, verify, vlen) != 0) {
			dev_err(cpe->dev,
				"CPE SRAM head verify mismatch @ 0x%x (type=%u): wrote %4phN read %4phN\n",
				paddr, mem_type, data, verify);
			return -EIO;
		}
		if (size > vlen) {
			size_t tail_off = size - vlen;

			if (!wcd9335_cpe_read_ram(cpe, mem_type, offset + tail_off,
						  verify, vlen) &&
			    memcmp(data + tail_off, verify, vlen) != 0) {
				dev_err(cpe->dev,
					"CPE SRAM tail verify mismatch @ 0x%zx (type=%u): wrote %4phN read %4phN\n",
					paddr + tail_off, mem_type,
					data + tail_off, verify);
				return -EIO;
			}
		}
	}
	return ret;
}

static int wcd9335_cpe_wait_outbox(struct wcd9335_cpe *cpe, u8 *outbox)
{
	unsigned long rem;
	unsigned int irq_st = 0;
	int i;

	rem = wait_for_completion_timeout(&cpe->outbox_compl, msecs_to_jiffies(1500));
	if (rem)
		return wcd9335_cpe_read_outbox(cpe, outbox);

	/* Fallback poll on INTR_PIN1_STATUS3 bit 5 */
	for (i = 0; i < 100; i++) {
		if (!regmap_read(cpe->regmap, WCD9335_INTR_PIN1_STATUS3, &irq_st) &&
		    (irq_st & BIT(5))) {
			regmap_write(cpe->regmap, WCD9335_INTR_PIN1_CLEAR3, BIT(5));
			return wcd9335_cpe_read_outbox(cpe, outbox);
		}
		usleep_range(5000, 6000);
	}

	return -ETIMEDOUT;
}

static int wcd9335_cpe_alloc_shmem(struct wcd9335_cpe *cpe)
{
	struct regmap *rm = cpe->regmap;
	u8 msg[WCD9335_CPE_MBOX_SIZE] = { 0 };
	struct wcd9335_cmi_hdr *hdr = (struct wcd9335_cmi_hdr *)msg;
	struct wcd9335_cmi_shmem_req *req =
		(struct wcd9335_cmi_shmem_req *)(msg + sizeof(*hdr));
	struct wcd9335_cmi_shmem_rsp *rsp;
	u8 outbox[WCD9335_CPE_MBOX_SIZE] = { 0 };
	unsigned int st = 0;
	int i, ret;

	for (i = 0; i < 10; i++) {
		regmap_read(rm, WCD9335_CPE_SS_STATUS, &st);
		if (!(st & BIT(2)))
			break;
		usleep_range(5000, 5500);
	}

	hdr->hdr_info = (CMI_CPE_CORE_SERVICE_ID << CMI_SVC_ID_SHIFT) & 0x70;
	hdr->pld_info = sizeof(*req) & 0x7f;
	hdr->opcode = cpu_to_le16(CMI_CORE_CMD_SHARED_MEM_ALLOC);
	req->size = cpu_to_le32(WCD9335_CPE_MSG_BUF_SIZE);

	reinit_completion(&cpe->outbox_compl);
	for (i = 0; i < WCD9335_CPE_MBOX_SIZE; i++) {
		ret = regmap_write(rm, WCD9335_CPE_SS_INBOX1_0 + i, msg[i]);
		if (ret)
			return ret;
	}

	ret = regmap_write(rm, WCD9335_CPE_SS_INBOX1_TRG, 0x01);
	if (ret)
		return ret;

	ret = wcd9335_cpe_wait_outbox(cpe, outbox);
	if (ret)
		return ret;

	hdr = (struct wcd9335_cmi_hdr *)outbox;
	if (le16_to_cpu(hdr->opcode) != CMI_CORE_RSP_SHARED_MEM_ALLOC) {
		dev_warn(cpe->dev, "CPE unexpected CMI response opcode 0x%04x\n",
			 le16_to_cpu(hdr->opcode));
		return -EPROTO;
	}

	rsp = (struct wcd9335_cmi_shmem_rsp *)(outbox + sizeof(*hdr));
	cpe->shmem_addr = le32_to_cpu(rsp->addr);
	return cpe->shmem_addr ? 0 : -ENOMEM;
}

static int wcd9335_cpe_load_segments(struct wcd9335_cpe *cpe,
				     const struct elf32_hdr *ehdr, bool exec_pass)
{
	const struct elf32_phdr *phdrs =
		(const struct elf32_phdr *)((const u8 *)ehdr + ehdr->e_phoff);
	int i, ret;

	for (i = 0; i < ehdr->e_phnum; i++) {
		const struct elf32_phdr *ph = &phdrs[i];
		const struct firmware *seg_fw;
		char seg_name[48];
		bool is_exec = !!(ph->p_flags & PF_X);
		bool is_rw = !is_exec && !!(ph->p_flags & (PF_R | PF_W));

		if ((exec_pass && !is_exec) || (!exec_pass && !is_rw))
			continue;

		if (ph->p_memsz == 0)
			continue;

		if (ph->p_filesz == 0) {
			if (!exec_pass && ph->p_paddr >= WCD9335_CPE_DRAM_BASE &&
			    (ph->p_paddr - WCD9335_CPE_DRAM_BASE) + ph->p_memsz <=
			    WCD9335_CPE_DRAM_SIZE) {
				static const u8 zero_chunk[WCD9335_CPE_CHUNK_SIZE] = { 0 };
				u32 off = ph->p_paddr - WCD9335_CPE_DRAM_BASE;
				u32 rem = ph->p_memsz;

				ret = wcd9335_cpe_set_mem_ptr(cpe, off,
					WCD9335_CPE_MEM_CTRL_AUTOINC | WCD9335_CPE_MEM_DRAM);
				while (!ret && rem > 0) {
					u32 n = min_t(u32, rem, sizeof(zero_chunk));

					ret = wcd9335_cpe_write_bytes(cpe, zero_chunk, n);
					rem -= n;
				}
				regmap_write(cpe->regmap, WCD9335_CPE_SS_MEM_CTRL, 0);
				if (ret)
					return ret;
			}
			continue;
		}

		snprintf(seg_name, sizeof(seg_name), "%s.b%02d", cpe->fw_name, i);
		ret = request_firmware(&seg_fw, seg_name, cpe->dev);
		if (ret) {
			dev_err(cpe->dev, "Missing CPE segment %s (%d)\n", seg_name, ret);
			return ret;
		}

		if (seg_fw->size != ph->p_filesz) {
			dev_err(cpe->dev, "%s size mismatch (%zu != %u)\n",
				seg_name, seg_fw->size, ph->p_filesz);
			release_firmware(seg_fw);
			return -EINVAL;
		}

		ret = wcd9335_cpe_write_segment(cpe, ph->p_paddr, ph->p_flags,
						seg_fw->data, seg_fw->size);
		release_firmware(seg_fw);
		if (ret)
			return ret;
	}

	return 0;
}

static void wcd9335_cpe_stop_locked(struct wcd9335_cpe *cpe)
{
	regmap_update_bits(cpe->regmap, WCD9335_CPE_SS_CPAR_CFG, 0x01, 0x00);
	regmap_update_bits(cpe->regmap, WCD9335_CPE_SS_CPAR_CTL, 0x07, 0x04);
	wcd9335_cpe_set_fll_clk(cpe, false);
	if (cpe->mclk_held) {
		cpe->mclk_ctrl(cpe->dev, false, NULL);
		cpe->mclk_held = false;
	}
	cpe->running = false;
}

static int wcd9335_cpe_boot_from_mdt(struct wcd9335_cpe *cpe, const struct firmware *fw)
{
	const struct elf32_hdr *ehdr;
	u8 outbox[WCD9335_CPE_MBOX_SIZE] = { 0 };
	struct wcd9335_cmi_hdr *hdr;
	struct wcd9335_cmi_boot_event *evt;
	unsigned int ss_status = 0;
	int ret;

	if (fw->size < sizeof(*ehdr))
		return -EINVAL;

	ehdr = (const struct elf32_hdr *)fw->data;
	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
	    ehdr->e_ident[EI_CLASS] != ELFCLASS32 ||
	    ehdr->e_machine != EM_XTENSA ||
	    ehdr->e_phnum == 0 ||
	    ehdr->e_phentsize != sizeof(struct elf32_phdr) ||
	    ehdr->e_phoff < sizeof(*ehdr) ||
	    size_add(ehdr->e_phoff, size_mul(ehdr->e_phnum, sizeof(struct elf32_phdr))) > fw->size) {
		dev_err(cpe->dev, "Invalid WCD9335 CPE ELF32/Xtensa MDT image\n");
		return -EINVAL;
	}

	if (cpe->running)
		wcd9335_cpe_stop_locked(cpe);

	ret = cpe->mclk_ctrl(cpe->dev, true, &cpe->mclk_rate);
	if (ret)
		return ret;
	cpe->mclk_held = true;

	ret = wcd9335_cpe_reset_hw(cpe);
	if (ret)
		goto err_stop;

	ret = wcd9335_cpe_load_segments(cpe, ehdr, true);
	if (ret)
		goto err_stop;

	ret = wcd9335_cpe_load_segments(cpe, ehdr, false);
	if (ret)
		goto err_stop;

	ret = wcd9335_cpe_set_fll_clk(cpe, true);
	if (ret)
		goto err_stop;

	reinit_completion(&cpe->outbox_compl);
	ret |= regmap_write(cpe->regmap, WCD9335_CPE_SS_SS_ERROR_INT_CLEAR, 0xff);
	ret |= regmap_write(cpe->regmap, WCD9335_CPE_SS_SS_ERROR_INT_MASK, 0x00);
	ret |= regmap_update_bits(cpe->regmap, WCD9335_CPE_SS_WDOG_CFG, 0x3f, 0x31);
	ret |= regmap_write(cpe->regmap, WCD9335_CPE_SS_CPARMAD_BUFRDY_INT_PERIOD, 19);
	ret |= regmap_update_bits(cpe->regmap, WCD9335_CPE_SS_CPAR_CTL, 0x04, 0x00);
	ret |= regmap_update_bits(cpe->regmap, WCD9335_CPE_SS_CPAR_CTL, 0x02, 0x02);
	ret |= regmap_update_bits(cpe->regmap, WCD9335_CPE_SS_CPAR_CTL, 0x01, 0x01);
	if (ret) {
		ret = -EIO;
		goto err_stop;
	}

	ret = wcd9335_cpe_wait_outbox(cpe, outbox);
	if (ret) {
		unsigned int rco = 0, clk_top = 0, err_st = 0;
		unsigned int fll[4] = { 0 }, pin1_st[4] = { 0 }, pin1_msk[4] = { 0 };
		unsigned int cpar_ctl = 0, cpar_cfg = 0, ss_cfg = 0, intr_cfg = 0;
		unsigned int vout_d = 0, buck_ctl = 0, mclk_cfg = 0, clk_gate = 0;
		u8 raw_outbox[WCD9335_CPE_MBOX_SIZE] = { 0 };
		int j;

		regmap_read(cpe->regmap, WCD9335_CPE_SS_STATUS, &ss_status);
		regmap_read(cpe->regmap, WCD9335_CPE_SS_CPAR_CTL, &cpar_ctl);
		regmap_read(cpe->regmap, WCD9335_CPE_SS_CPAR_CFG, &cpar_cfg);
		regmap_read(cpe->regmap, WCD9335_CPE_SS_CFG, &ss_cfg);
		regmap_read(cpe->regmap, WCD9335_ANA_RCO, &rco);
		regmap_read(cpe->regmap, WCD9335_ANA_CLK_TOP, &clk_top);
		regmap_read(cpe->regmap, WCD9335_ANA_BUCK_VOUT_D, &vout_d);
		regmap_read(cpe->regmap, WCD9335_ANA_BUCK_CTL, &buck_ctl);
		regmap_read(cpe->regmap, WCD9335_CODEC_RPM_CLK_GATE, &clk_gate);
		regmap_read(cpe->regmap, WCD9335_CODEC_RPM_CLK_MCLK_CFG, &mclk_cfg);
		regmap_read(cpe->regmap, WCD9335_INTR_CFG, &intr_cfg);
		for (j = 0; j < 4; j++) {
			regmap_read(cpe->regmap, WCD9335_CPE_FLL_STATUS_0 + j, &fll[j]);
			regmap_read(cpe->regmap, WCD9335_INTR_PIN1_STATUS0 + j, &pin1_st[j]);
			regmap_read(cpe->regmap, WCD9335_INTR_PIN1_MASK0 + j, &pin1_msk[j]);
		}
		regmap_read(cpe->regmap, WCD9335_CPE_SS_SS_ERROR_INT_STATUS, &err_st);
		for (j = 0; j < WCD9335_CPE_MBOX_SIZE; j++) {
			unsigned int v = 0;

			regmap_read(cpe->regmap, WCD9335_CPE_SS_OUTBOX1_0 + j, &v);
			raw_outbox[j] = v & 0xff;
		}
		dev_err(cpe->dev,
			"CPE boot timeout: ss=0x%02x cpar=0x%02x/0x%02x cfg=0x%02x rco=0x%02x top=0x%02x fll=%02x.%02x.%02x.%02x buck=%02x/%02x clk=%02x/%02x err=0x%02x intr=%02x st=%02x.%02x.%02x.%02x msk=%02x.%02x.%02x.%02x ob=%16phN\n",
			ss_status, cpar_ctl, cpar_cfg, ss_cfg, rco, clk_top,
			fll[0], fll[1], fll[2], fll[3], vout_d, buck_ctl,
			clk_gate, mclk_cfg, err_st, intr_cfg,
			pin1_st[0], pin1_st[1], pin1_st[2], pin1_st[3],
			pin1_msk[0], pin1_msk[1], pin1_msk[2], pin1_msk[3],
			raw_outbox);
		goto err_stop;
	}

	hdr = (struct wcd9335_cmi_hdr *)outbox;
	if (le16_to_cpu(hdr->opcode) != CMI_CORE_EVENT_SYSTEM_BOOT) {
		dev_err(cpe->dev, "Unexpected CPE boot opcode 0x%04x\n",
			le16_to_cpu(hdr->opcode));
		ret = -EPROTO;
		goto err_stop;
	}

	evt = (struct wcd9335_cmi_boot_event *)(outbox + sizeof(*hdr));
	cpe->boot_status = evt->status;
	cpe->cmi_version = evt->version;
	cpe->sfr_size = le16_to_cpu(evt->sfr_buff_size);
	cpe->sfr_addr = le32_to_cpu(evt->sfr_buff_address);

	if (evt->status != 0) {
		dev_err(cpe->dev, "CPE firmware reported boot error %u\n", evt->status);
		ret = -EIO;
		goto err_stop;
	}

	ret = wcd9335_cpe_alloc_shmem(cpe);
	if (ret) {
		dev_err(cpe->dev, "CPE CMI shmem alloc failed (%d)\n", ret);
		goto err_stop;
	}

	regmap_read(cpe->regmap, WCD9335_CPE_SS_STATUS, &cpe->last_ss_status);
	cpe->running = true;
	dev_info(cpe->dev,
		 "WCD9335 CPE booted (%s.mdt): cmi_ver=%u sfr=0x%x/%uB shmem=0x%x ss=0x%02x\n",
		 cpe->fw_name, cpe->cmi_version, cpe->sfr_addr,
		 cpe->sfr_size, cpe->shmem_addr, cpe->last_ss_status);
	return 0;

err_stop:
	wcd9335_cpe_stop_locked(cpe);
	return ret;
}

static int wcd9335_cpe_start_sync(struct wcd9335_cpe *cpe)
{
	const struct firmware *fw;
	char mdt_name[48];
	int ret;

	snprintf(mdt_name, sizeof(mdt_name), "%s.mdt", cpe->fw_name);
	ret = request_firmware(&fw, mdt_name, cpe->dev);
	if (ret) {
		dev_info(cpe->dev, "Firmware %s unavailable (%d)\n", mdt_name, ret);
		return ret;
	}

	ret = wcd9335_cpe_boot_from_mdt(cpe, fw);
	release_firmware(fw);
	return ret;
}

static void wcd9335_cpe_fw_cb(const struct firmware *fw, void *context)
{
	struct wcd9335_cpe *cpe = context;

	if (!fw)
		return;

	mutex_lock(&cpe->lock);
	if (!cpe->running)
		wcd9335_cpe_boot_from_mdt(cpe, fw);
	mutex_unlock(&cpe->lock);
	release_firmware(fw);
}

static ssize_t cpe_load_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct wcd9335_cpe *cpe = wcd9335_dev_get_cpe(dev);

	if (!cpe)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", cpe->running ? 1 : 0);
}

static ssize_t cpe_load_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct wcd9335_cpe *cpe = wcd9335_dev_get_cpe(dev);
	bool enable;
	int ret;

	if (!cpe)
		return -ENODEV;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	mutex_lock(&cpe->lock);
	if (enable)
		ret = cpe->running ? 0 : wcd9335_cpe_start_sync(cpe);
	else if (cpe->running)
		wcd9335_cpe_stop_locked(cpe);
	mutex_unlock(&cpe->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(cpe_load);

static ssize_t cpe_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct wcd9335_cpe *cpe = wcd9335_dev_get_cpe(dev);
	unsigned int ss_status = 0;

	if (!cpe)
		return -ENODEV;

	mutex_lock(&cpe->lock);
	if (cpe->running)
		regmap_read(cpe->regmap, WCD9335_CPE_SS_STATUS, &ss_status);
	else
		ss_status = cpe->last_ss_status;
	mutex_unlock(&cpe->lock);

	return sysfs_emit(buf,
			  "running=%d fw=%s mclk=%u ss_status=0x%02x cmi_ver=%u boot_status=%u sfr=0x%x/%u shmem=0x%x errs=%u\n",
			  cpe->running ? 1 : 0, cpe->fw_name, cpe->mclk_rate, ss_status,
			  cpe->cmi_version, cpe->boot_status, cpe->sfr_addr,
			  cpe->sfr_size, cpe->shmem_addr, cpe->err_count);
}
static DEVICE_ATTR_RO(cpe_status);

static struct attribute *wcd9335_cpe_attrs[] = {
	&dev_attr_cpe_load.attr,
	&dev_attr_cpe_status.attr,
	NULL,
};
static const struct attribute_group wcd9335_cpe_group = {
	.attrs = wcd9335_cpe_attrs,
};

struct wcd9335_cpe *wcd9335_cpe_init(struct device *dev, struct regmap *regmap,
				     int virq_outbox1, int virq_err,
				     int (*mclk_ctrl)(struct device *, bool, u32 *))
{
	struct wcd9335_cpe *cpe;
	int ret;

	cpe = devm_kzalloc(dev, sizeof(*cpe), GFP_KERNEL);
	if (!cpe)
		return ERR_PTR(-ENOMEM);

	cpe->dev = dev;
	cpe->regmap = regmap;
	cpe->virq_outbox1 = virq_outbox1;
	cpe->virq_err = virq_err;
	cpe->mclk_ctrl = mclk_ctrl;
	strscpy(cpe->fw_name, WCD9335_CPE_DEFAULT_FW, sizeof(cpe->fw_name));
	mutex_init(&cpe->lock);
	init_completion(&cpe->outbox_compl);

	if (virq_outbox1 > 0) {
		ret = devm_request_threaded_irq(dev, virq_outbox1, NULL,
						wcd9335_cpe_outbox_irq,
						IRQF_ONESHOT,
						"wcd9335-cpe-outbox", cpe);
		if (ret)
			dev_warn(dev, "Failed to request CPE OUTBOX1 IRQ (%d)\n", ret);
	}

	if (virq_err > 0) {
		ret = devm_request_threaded_irq(dev, virq_err, NULL,
						wcd9335_cpe_err_irq,
						IRQF_ONESHOT,
						"wcd9335-cpe-err", cpe);
		if (ret)
			dev_warn(dev, "Failed to request CPE ERR IRQ (%d)\n", ret);
	}

	ret = devm_device_add_group(dev, &wcd9335_cpe_group);
	if (ret)
		return ERR_PTR(ret);

	return cpe;
}

void wcd9335_cpe_request_fw(struct wcd9335_cpe *cpe)
{
	char mdt_name[48];

	if (!cpe || cpe->running)
		return;

	snprintf(mdt_name, sizeof(mdt_name), "%s.mdt", cpe->fw_name);
	request_firmware_nowait(THIS_MODULE, true, mdt_name, cpe->dev,
				GFP_KERNEL, cpe, wcd9335_cpe_fw_cb);
}

void wcd9335_cpe_deinit(struct wcd9335_cpe *cpe)
{
	if (!cpe)
		return;

	mutex_lock(&cpe->lock);
	if (cpe->running)
		wcd9335_cpe_stop_locked(cpe);
	mutex_unlock(&cpe->lock);
}
