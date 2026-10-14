#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Recovery adapter for topjohnwu/Magisk v31.0. The bundled APK is unchanged.
# Invoked with Magisk's own BusyBox, in a private mount namespace.

fail() {
  echo "ui_print ! $1" >&2
  exit 1
}

[ "$1" = 3 ] && [ -n "$2" ] && [ -f "$3" ] || fail "Invalid installer arguments"
[ -n "$EVO_MAGISK_WORK" ] && [ -x "$BBBIN" ] || fail "Missing RAM workspace"
# Metadata is supplied by the native runner before /system is covered by Android.
case "$EVO_MAGISK_API" in ''|*[!0-9]*) fail "Missing Android API metadata";; esac
[ "$EVO_MAGISK_API" -ge 23 ] || fail "Unsupported Android API"
case "$EVO_MAGISK_ABI" in
  arm64-v8a|armeabi-v7a|x86_64|x86) ;;
  *) fail "Unsupported or missing recovery ABI";;
esac
export TMPDIR="$EVO_MAGISK_WORK/tmp"
export INSTALLER="$EVO_MAGISK_WORK/install"
export BOOTMODE=false
mkdir -p "$TMPDIR" "$INSTALLER" || fail "Cannot create RAM workspace"
unzip -o "$3" 'assets/*' 'lib/*' 'META-INF/com/google/*' \
  -x 'lib/*/libbusybox.so' -d "$INSTALLER" >&2 || fail "Cannot extract Magisk"

# Upstream normally mounts /data and falls back to /cache for its workspace.
# EvoX recovery may have neither. Use RAM and retain the upstream boot patcher.
UTIL="$INSTALLER/assets/util_functions.sh"
SCRIPT="$INSTALLER/META-INF/com/google/android/updater-script"
[ -f "$UTIL" ] && [ -f "$SCRIPT" ] || fail "Missing Magisk installer scripts"
grep -qx 'is_mounted /data || mount /data || is_mounted /cache || mount /cache' "$SCRIPT" \
  || fail "Unsupported Magisk installer layout"
sed '/^is_mounted \/data || mount \/data || is_mounted \/cache || mount \/cache$/d' \
  "$SCRIPT" > "$INSTALLER/ramdisk-installer.sh" || fail "Cannot adapt installer"
cat >> "$UTIL" <<'RAM_ADAPTER'

# EvoX recovery: never require, mount, or stage installation files on userdata/cache.
TMPDIR="$EVO_MAGISK_WORK/tmp"
check_data() {
  DATA=false
  DATA_DE=false
  MAGISKBIN="$TMPDIR/magisk"
}
# Android's /system may not expose recovery's getprop or product properties.
# Never run an architecture-less boot patcher; use the native metadata snapshot.
api_level_arch_detect() {
  API="$EVO_MAGISK_API"
  ABI="$EVO_MAGISK_ABI"
  case "$ABI" in
    arm64-v8a) ARCH=arm64; ABI32=armeabi-v7a; IS64BIT=true;;
    armeabi-v7a) ARCH=arm; ABI32=armeabi-v7a; IS64BIT=false;;
    x86_64) ARCH=x64; ABI32=x86; IS64BIT=true;;
    x86) ARCH=x86; ABI32=x86; IS64BIT=false;;
  esac
}
# Use upstream safe defaults rather than reading storage configuration.
getvar() { :; }
# No persistent stock-boot backups or module migration without userdata access.
run_migrations() { :; }
RAM_ADAPTER

exec "$BBBIN" sh "$INSTALLER/ramdisk-installer.sh" "$@"
