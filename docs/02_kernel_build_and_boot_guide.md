# Mainline Linux 6.18 Build, Packaging & Verification Guide (`dumpling`)

## Toolchain & Host Requirements
- Host: Ubuntu 26.04 x86_64 (`192.168.1.15`)
- Cross compiler: `aarch64-linux-gnu-gcc` (`CROSS_COMPILE=aarch64-linux-gnu-`)
- Required for `CONFIG_DEBUG_INFO_BTF=y`: `pahole` (`dwarves`)

## Build Kernel #62+ (`Image.gz-dtb` + Modules)
```bash
cd ~/oneplus5t-android17/kernel/linux-mainline-6.18
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- oneplus5t_android17_defconfig
make -j"$(nproc)" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" \
  Image.gz qcom/msm8998-oneplus-dumpling.dtb modules

rm -rf /tmp/op5t_mods && mkdir -p /tmp/op5t_mods
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  INSTALL_MOD_PATH=/tmp/op5t_mods INSTALL_MOD_STRIP=1 modules_install

cat arch/arm64/boot/Image.gz arch/arm64/boot/dts/qcom/msm8998-oneplus-dumpling.dtb > /tmp/Image.gz-dtb
aarch64-linux-gnu-readelf -S vmlinux | grep -E "BTF|btf"
```

## Deploy Kernel & Modules to Android ROM Device Tree
Copy `/tmp/Image.gz-dtb` to `device/oneplus/dumpling/prebuilt/kernel/Image.gz-dtb` and the stripped `.ko` files (`find /tmp/op5t_mods -name "*.ko"`) to `device/oneplus/dumpling/prebuilt/kernel/modules/`.

## Safe RAM Boot Test (`mkbootimg` + `fastboot boot`)
```bash
mkbootimg \
  --kernel /tmp/Image.gz-dtb \
  --ramdisk initramfs.cpio.gz \
  --base 0x00000000 \
  --kernel_offset 0x00008000 \
  --ramdisk_offset 0x01000000 \
  --tags_offset 0x00000100 \
  --pagesize 4096 \
  --cmdline "console=tty0 androidboot.hardware=qcom androidboot.console=ttyMSM0 earlycon" \
  -o /tmp/boot-6.18-test.img

fastboot boot /tmp/boot-6.18-test.img
```
*Reminder:* `abl.img` enforces `EndOfKernelRegion = 0x85600000` (`86 MiB` total for decompressed `Image` + DTB + ramdisk + gzip scratch). Keep `adb` / `fastboot` unlocked during testing.
