# OnePlus 5T (`dumpling`) — Mainline Linux 6.18 Kernel Port

> [!CAUTION]
> **DISCLAIMER OF LIABILITY**
>
> **THIS KERNEL AND REPOSITORY ARE PROVIDED "AS-IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.**
> **NEITHER THE REPOSITORY OWNER NOR ANY CONTRIBUTORS ARE RESPONSIBLE OR LIABLE FOR DEAD OR BRICKED PHONES, HARDWARE DAMAGE, BOOTLOOPING, VOIDED WARRANTIES, LOSS OF MODEM/EFS/PERSIST DATA, OR ANY OTHER DATA LOSS.**
> Flashing or booting custom kernels carries inherent risk. Always back up your `persist`, `modemst1`, `modemst2`, `fsg`, `param`, and EFS partitions first, and validate builds via RAM boot (`fastboot boot`) before touching persistent partitions.

> [!NOTE]
> **AI DISCLOSURE**
>
> AI assistance was used to help create this repository, reverse-engineer/port clean-room drivers against stock device trees and kernel sources, debug hardware bring-up, and author documentation.

---

## Overview

This repository provides a mainline **Linux 6.18** (`dumpling-6.18-android17`, branched from `upstream-6.18`) kernel port for the **OnePlus 5T (`dumpling`, Qualcomm MSM8998)** targeting clean-room Android 17 / AOSP / Evolution X bring-up and upstream mainline feature parity.

## Branches

- `dumpling-6.18-android17` — Active OnePlus 5T (`dumpling`) mainline 6.18 port, custom DT/defconfig, and clean-room drivers.
- `upstream-6.18` — Clean Linux 6.18 upstream baseline.

## Ported Subsystems & Verified Hardware Features

- **Core SoC, CPUFreq & Interconnect:** Clean-room MSM8998 OSM v2, CPRh closed-loop AVS, SAW4 AVS CPUFreq (LITTLE 300–1900.8 MHz / big 300–2457.6 MHz), and NoC + BWMON interconnect scaling.
- **TrustZone & Storage Encryption / RPMB:** Clean-room `/dev/qseecom` (`qcom_scm` bridge + reserved `qseecom-region`), `UFS_IOCTL_QUERY` + SCSI Generic `/dev/sg*` (`CONFIG_CHR_DEV_SG`) for RPMB, UFS Inline Crypto Engine (ICE), Qualcomm Crypto Engine (`qce`), and TZ PRNG (`qcom-rng`).
- **Biometrics (Fingerprint):** Clean-room Goodix GF5288 + ID detector (`oneplus,fpdetect`, `/dev/goodix_fp`, Netlink group 25, `gf_input`, `sensor_version=0x03 (gfp5288)`).
- **Audio & WCD9335 CPE:** QDSP6/ADSP APR/GLINK audio (`0 [O5T]: msm8998-snd`), WCD9335 codec over SLIMbus, clean-room WCD9335 CPE Xtensa DSP loader (`cpe_9335.mdt`, CMI boot + `cpe_load` / `cpe_status`), and TFA989x/TFA9890 speaker amplifier support.
- **Video Encode/Decode (Track 4):** Qualcomm Venus (`venus.mdt`) hardware video decoder (`/dev/video6`) and encoder (`/dev/video7`) with MSM8998 VA bounds, subcore GDSC/hwmode sequencing, `IS_V3` zero-buffer EOS handling, `parse_raw_formats()` HEVC cap parsing fix, and H264 8x8 transform property (`H264`, `HEVC`, `VP8`, `MPEG4`, `H263`, and `1080p` `H264`/`HEVC` live-verified).
- **Dash / VOOC Fast Charge (`oneplus_dash`):** Clean-room STM8S003F3U6TR driver (`oneplus,dash-charger` @ `4-0026`) exposing `/dev/dash` and `power_supply/oneplus-dash`, verifying `oneplus/dash_stm8s.bin` via the post-reset bootloader window while keeping `usb_switch=0` (`gpio 90/91=0`).
- **Remote Processors & Modems:** Modem (`mba.mbn` + `modem.mdt`), ADSP (`adsp.mdt`), SLPI (`slpi.mdt`), IPA v3.1 (`ipa_fws.mbn` + `rmnet_ipa0`), QRTR + `pd-mapper` + SMP2P AP sleepstate.
- **Camera Pipeline (CAMSS):** CSIPHY/CSID/ISPIF/VFE media graph + Sony IMX398 / IMX376K / IMX371 clean-room sensor drivers, `lc898217xc` VCM actuator drivers, and `24c64` EEPROMs.
- **Display, GPU, Input & Connectivity:** Samsung S6E3FC1 1080×2160 AMOLED DSI panel (configured with 5% default boot brightness to prevent burn-in during bring-up), Adreno 540 DRM/MSM (`a530_pfp`/`pm4` + `a540_gpmu`), Synaptics S3508 touchscreen (with DT2W), SPMI haptics, tri-state alert slider, Hall sensor, PM8998 flash/RGB LEDs, `bq27411` fuel gauge, WCN3990 Wi-Fi (`ath10k_snoc` / `wlan0`), Bluetooth (`hci0`), NXP NCI NFC (`nfc0`), and IMEM `syscon-reboot-mode` (`reboot bootloader` / `reboot recovery`).
- **Android 17 Kernel Features:** BinderFS, Ashmem/memfd, strict DMA-IOMMU, PSI, EROFS/F2FS/dm-verity, BPF/netfilter, and USB ConfigFS gadget (RNDIS + CDC ACM + FunctionFS diag).

## Remaining Non-Ported / Hardware-Gated Stock Items

Some stock vendor blobs cannot operate or remain out-of-tree under a clean-room mainline kernel without OnePlus/Qualcomm private protocols or locked-bootloader TrustZone boundaries:

- **Widevine L1 & HDCP (`hdcp*.mdt`):** `hdcp*` firmware images are absent from this device's stock firmware partition, and an unlocked bootloader / TrustZone XPU boundary blocks OEM Widevine L1 keybox provisioning (Widevine L3 works).
- **NXP P61 SPI eSE (`nxp,p61` on `blsp1_spi1`):** Clean-room `nxp_p61_ese` driver implemented, but `GPIOs 0..3` (`gpio-reserved-ranges = <0 4>`) are TZ XPU protected on production firmware so `&blsp1_spi1` stays `disabled` in DT; standard NFC uses `nxp-nci` on I2C (`nfc0`).
- **Hardware-N/A Stock Reference Stubs:** Upstream stock DTs/makefiles list other-device targets (`fpc1020`, `cpe_9340`, `a630_gmu`, `isdbtmm`, `keypad-backlight`, etc.) that are not present on OnePlus 5T (`dumpling`) hardware.

## Building

```bash
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-

make oneplus5t_android17_defconfig
make -j"$(nproc)" Image.gz dtbs modules
```

Output artifacts:
- Kernel image: `arch/arm64/boot/Image.gz`
- Device Tree: `arch/arm64/boot/dts/qcom/msm8998-oneplus-dumpling.dtb`
- Combined image: `cat arch/arm64/boot/Image.gz arch/arm64/boot/dts/qcom/msm8998-oneplus-dumpling.dtb > Image.gz-dtb`

Test safely in RAM before ever flashing:

```bash
fastboot boot boot.img
```

For full OnePlus 5T stock firmware & clean-room Android 17 integration notes, consult your firmware extraction setup (`/vendor/firmware_mnt/image` and `/vendor/firmware`).
---

## Android 17 / Kernel #62 Updates & Repository Knowledge Docs

- **Commits `8700ef046`..`8f55c2473` (#62):**
  - DPU < 5.0 command-mode TE synchronization (`_dpu_encoder_phys_cmd_wait_for_te` / `wait_for_idle`) + Samsung S6E3FC1 HS brightness writes and `srgb`/`dci_p3`/`hbm` sysfs nodes.
  - Adreno 540 shared global TTBR0 `gpu->vm` workaround (`create_private_vm = NULL`, `qcom_adreno_can_do_ttbr1 = false`, deferred global VMA teardown on close), restored `342–710 MHz` OPPs, and `pm8005_s1` min `1.088 V`.
  - Android 17 eBPF tracing/BTF support (`CONFIG_FTRACE`, `CONFIG_TRACING`, `CONFIG_EVENT_TRACING`, `CONFIG_KPROBES`, `CONFIG_BPF_EVENTS`, `CONFIG_DEBUG_INFO_DWARF5`, `CONFIG_DEBUG_INFO_BTF`) enabling `.BTF`/`.BTF_ids` for `BpfLoader-rs`.
- **Engineering Documentation:**
  - [Stock 4.4.205 vs Mainline 6.18 Architecture & Findings](docs/01_stock_4_4_vs_mainline_6_18_findings.md)
  - [Kernel Build, Packaging & Verification Guide](docs/02_kernel_build_and_boot_guide.md)
  - [Mainline MSM8998 Kernel Skill](skills/msm8998-mainline-android-kernel/SKILL.md)
