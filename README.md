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
- **Video Encode/Decode:** Qualcomm Venus (`venus.mdt`) hardware video decoder (`/dev/video6`) and encoder (`/dev/video7`) with MSM8998 VA bounds and subcore GDSC sequencing.
- **Remote Processors & Modems:** Modem (`mba.mbn` + `modem.mdt`), ADSP (`adsp.mdt`), SLPI (`slpi.mdt`), IPA v3.1 (`ipa_fws.mbn` + `rmnet_ipa0`), QRTR + `pd-mapper` + SMP2P AP sleepstate.
- **Camera Pipeline (CAMSS):** CSIPHY/CSID/ISPIF/VFE media graph + Sony IMX398 / IMX376K / IMX371 clean-room sensor drivers, `lc898217xc` VCM actuator drivers, and `24c64` EEPROMs.
- **Display, GPU, Input & Connectivity:** Samsung S6E3FC1 1080×2160 AMOLED DSI panel (configured with 5% default boot brightness to prevent burn-in during bring-up), Adreno 540 DRM/MSM (`a530_pfp`/`pm4` + `a540_gpmu`), Synaptics S3508 touchscreen (with DT2W), SPMI haptics, tri-state alert slider, Hall sensor, PM8998 flash/RGB LEDs, `bq27411` fuel gauge, WCN3990 Wi-Fi (`ath10k_snoc` / `wlan0`), Bluetooth (`hci0`), NXP NCI NFC (`nfc0`), and IMEM `syscon-reboot-mode` (`reboot bootloader` / `reboot recovery`).
- **Android 17 Kernel Features:** BinderFS, Ashmem/memfd, strict DMA-IOMMU, PSI, EROFS/F2FS/dm-verity, BPF/netfilter, and USB ConfigFS gadget (RNDIS + CDC ACM + FunctionFS diag).

## Remaining Non-Ported / Hardware-Gated Stock Items

Some stock vendor blobs cannot operate or remain out-of-tree under a clean-room mainline kernel without OnePlus/Qualcomm private protocols or locked-bootloader TrustZone boundaries:

- **Dash / VOOC Fast Charge MCU (`microchip,oneplus_fastcg`, I2C `0x26`):** Standard USB-C / PM8998 charging works; proprietary VOOC handshake firmware is not ported.
- **Widevine L1 & HDCP (`hdcp*.mdt`):** `hdcp*` firmware images are absent from this device's stock firmware partition, and an unlocked bootloader / TrustZone XPU boundary blocks OEM Widevine L1 keybox provisioning (Widevine L3 works).
- **Optional NXP P61 SPI eSE (`nxp,p61`):** Not required for standard NFC (`nxp-nci` on I2C handles standard NFC) or consumer operation.
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
