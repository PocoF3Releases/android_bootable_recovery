#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Checked recovery adapter for the unmodified official APatch 11224 tools.

fail() {
  echo "ui_print ! $1" >&2
  exit 1
}

[ "$1" = 3 ] && [ -n "$2" ] && [ -f "$3" ] || fail "Invalid installer arguments"
[ -n "$EVO_MAGISK_WORK" ] && [ -x "$BBBIN" ] || fail "Missing RAM workspace"
[ "$EVO_MAGISK_ABI" = arm64-v8a ] || fail "APatch requires ARM64"
case "$EVO_APATCH_BOOT" in
  /dev/block/by-name/boot|/dev/block/by-name/boot_a|/dev/block/by-name/boot_b) ;;
  *) fail "Invalid boot partition";;
esac
[ -b "$EVO_APATCH_BOOT" ] || fail "Boot partition is unavailable"

export TMPDIR="$EVO_MAGISK_WORK/tmp"
export INSTALLER="$EVO_MAGISK_WORK/install"
umask 077
mkdir -p "$TMPDIR" "$INSTALLER" || fail "Cannot create RAM workspace"
unzip -o "$3" 'assets/kpimg' 'lib/arm64-v8a/libkptools.so' \
  -d "$INSTALLER" >&2 || fail "Cannot extract APatch tools"
cd "$INSTALLER" || fail "Cannot enter RAM workspace"
cp lib/arm64-v8a/libkptools.so kptools || fail "Cannot stage patcher"
chmod 700 kptools || fail "Cannot enable patcher"
[ -s assets/kpimg ] || fail "KernelPatch payload is missing"

# Snapshot the current boot partition. Never write init_boot or userdata.
echo "ui_print Reading active boot image"
dd if="$EVO_APATCH_BOOT" of=boot.img bs=1048576 >&2 || fail "Cannot read boot image"
./kptools unpack boot.img >unpack.log 2>&1 || fail "Cannot unpack boot image"
[ -s kernel ] || fail "Boot image contains no supported kernel"
./kptools -i kernel -f >kernel.config 2>&1 || fail "Cannot inspect kernel configuration"
grep -qx 'CONFIG_KALLSYMS=y' kernel.config || fail "Kernel requires CONFIG_KALLSYMS"
./kptools -i kernel -l >kernel.info 2>&1 || fail "Cannot inspect kernel patches"
grep -qx 'patched=false' kernel.info || fail "Use an original, unpatched boot image"
version=$(sed -n 's/^banner=Linux version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p' kernel.info)
set -- $version
[ "$#" = 2 ] || fail "Cannot determine target kernel version"
case "$1" in
  3) [ "$2" -ge 18 ] || fail "Unsupported kernel version";;
  4|5) ;;
  6) [ "$2" -le 12 ] || fail "Unsupported kernel version";;
  *) fail "Unsupported kernel version";;
esac

# Inspect the original ramdisk with official MagiskBoot. Keep its unpacked
# files separate from KernelPatch's kernel and never stack Magisk installations.
mkdir inspect || fail "Cannot prepare boot inspection"
(
  cd inspect || exit 1
  "$EVO_MAGISK_WORK/magiskboot" unpack ../boot.img >inspect.log 2>&1 || exit 1
  if [ -f ramdisk.cpio ]; then
    "$EVO_MAGISK_WORK/magiskboot" cpio ramdisk.cpio test >/dev/null 2>&1 || exit 1
  fi
) || fail "Use a supported stock boot image without Magisk"
mv kernel kernel-origin || fail "Cannot preserve original kernel"
echo "ui_print Patching kernel with APatch"
# Use upstream direct-install key generation; no user credential is logged.
./kptools -p --image kernel-origin --kpimg assets/kpimg --out kernel \
  >patch.log 2>&1 || fail "KernelPatch rejected this kernel"
[ -s kernel ] || fail "Patcher produced no kernel"
./kptools -i kernel -l >patched.info 2>&1 || fail "Cannot verify patched kernel"
grep -qx 'patched=true' patched.info || fail "Kernel patch verification failed"
./kptools repack boot.img >repack.log 2>&1 || fail "Cannot repack boot image"
[ -s new-boot.img ] || fail "Repacker produced no boot image"
original_size=$(wc -c <boot.img)
patched_size=$(wc -c <new-boot.img)
[ "$patched_size" -le "$original_size" ] || fail "Patched image exceeds boot partition"
echo "ui_print Writing verified boot image"
dd if=new-boot.img of="$EVO_APATCH_BOOT" bs=1048576 conv=fsync >&2 || \
  fail "Boot write failed; keep recovery open and restore the original boot image"
# Read back exactly the bytes written, with no padded-partition hash mismatch.
head -c "$patched_size" "$EVO_APATCH_BOOT" | cmp - new-boot.img || \
  fail "Boot readback failed; restore the original boot image"
echo "ui_print APatch installed. Install APatch Manager after reboot."
echo "ui_print No persistent stock-boot backup was created."
