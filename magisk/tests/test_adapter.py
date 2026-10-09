#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Host adapter tests: never run the real boot patcher or access Android partitions."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT / "Magisk-v31.0.apk"
EXPECTED = "2c8a488b9a5293e578e95ae4f07e3c57aba4feec4a52ca4dd852a2692d6dd4e8"
MOUNT = "is_mounted /data || mount /data || is_mounted /cache || mount /cache"

# Real v31.0 utilities, but a fixture entrypoint replaces all partition operations.
# The marker proves the original mounting line was removed, with no host mounts.
ENTRYPOINT = r"""#!/system/bin/sh
OUTFD=$2
COMMONDIR=$INSTALLER/assets
. "$COMMONDIR/util_functions.sh"
is_mounted /data || mount /data || is_mounted /cache || mount /cache
# Property lookups through the mounted Android filesystem must not be needed.
grep_get_prop() { exit 25; }
api_level_arch_detect
[ "$API" = "$EVO_MAGISK_API" ] && [ "$ABI" = "$EVO_MAGISK_ABI" ] || exit 26
case "$ABI" in
  arm64-v8a) [ "$ARCH:$ABI32:$IS64BIT" = 'arm64:armeabi-v7a:true' ] || exit 27;;
  armeabi-v7a) [ "$ARCH:$ABI32:$IS64BIT" = 'arm:armeabi-v7a:false' ] || exit 27;;
  x86_64) [ "$ARCH:$ABI32:$IS64BIT" = 'x64:x86:true' ] || exit 27;;
  x86) [ "$ARCH:$ABI32:$IS64BIT" = 'x86:x86:false' ] || exit 27;;
esac
check_data
[ "$DATA" = false ] && [ "$DATA_DE" = false ] || exit 20
[ "$MAGISKBIN" = "$EVO_MAGISK_WORK/tmp/magisk" ] || exit 21
[ "$TMPDIR" = "$EVO_MAGISK_WORK/tmp" ] || exit 22
[ "$BOOTMODE" = false ] || exit 23
# Detect accidental use of the upstream storage-reading helpers.
grep_prop() { exit 24; }
getvar KEEPVERITY
run_migrations
echo "ui_print fixture reached" >&$OUTFD
printf '%s\n' "$1" "$3" "$MAGISKBIN" > "$EVO_MAGISK_WORK/reached"
exit ${FIXTURE_STATUS:-0}
"""

class AdapterTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="evox magisk test ")
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        with zipfile.ZipFile(APK) as archive:
            self.util = archive.read("assets/util_functions.sh")
            self.busybox = self.work / "busybox"
            self.busybox.write_bytes(archive.read("lib/x86_64/libbusybox.so"))
        self.busybox.chmod(0o700)
        self.package = self.work / "fixture.apk"

    def fixture(self, entrypoint=ENTRYPOINT):
        with zipfile.ZipFile(self.package, "w") as archive:
            archive.writestr("assets/util_functions.sh", self.util)
            archive.writestr("META-INF/com/google/android/updater-script", entrypoint)

    def run_adapter(self, status="0", abi="x86_64", api="37"):
        env = {**os.environ, "EVO_MAGISK_WORK": str(self.work),
               "BBBIN": str(self.busybox), "ASH_STANDALONE": "1", "FIXTURE_STATUS": status,
               "EVO_MAGISK_ABI": abi, "EVO_MAGISK_API": api}
        return subprocess.run([str(self.busybox), "sh", str(ROOT / "install-magisk.sh"),
                               "3", "1", str(self.package)], env=env,
                              capture_output=True, text=True, timeout=15)

    def test_official_package_integrity(self):
        self.assertEqual(hashlib.sha256(APK.read_bytes()).hexdigest(), EXPECTED)
        with zipfile.ZipFile(APK) as archive:
            self.assertIsNone(archive.testzip())
            script = archive.read("META-INF/com/google/android/updater-script").decode()
            self.assertIn(MOUNT, script.splitlines())
            for name in ("check_data", "getvar", "run_migrations"):
                self.assertIn((name + "() {").encode(), self.util)

    def test_ram_workspace_and_protocol_with_spaces(self):
        self.fixture()
        result = self.run_adapter()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ui_print fixture reached", result.stdout)
        self.assertEqual((self.work / "reached").read_text().splitlines(),
                         ["3", str(self.package), str(self.work / "tmp/magisk")])

    def test_all_bundled_tools_are_extracted(self):
        # Exercise the real extraction command with every upstream payload entry,
        # but replace only the entrypoint so no partition operation is executed.
        with zipfile.ZipFile(APK) as official, zipfile.ZipFile(self.package, "w") as fixture:
            for name in official.namelist():
                if name.startswith(("assets/", "lib/")):
                    fixture.writestr(name, official.read(name))
            fixture.writestr("META-INF/com/google/android/updater-script", ENTRYPOINT)
        result = self.run_adapter()
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(APK) as official:
            for name in official.namelist():
                if not name.startswith(("assets/", "lib/")):
                    continue
                if name.endswith("libbusybox.so") or name == "assets/util_functions.sh":
                    continue
                staged = self.work / "install" / name
                self.assertTrue(staged.is_file(), name)
                self.assertEqual(staged.read_bytes(), official.read(name), name)
            for abi in ("arm64-v8a", "armeabi-v7a"):
                for tool in ("bootctl", "magisk", "magiskboot", "magiskinit", "magiskpolicy", "init-ld"):
                    self.assertTrue((self.work / "install/lib" / abi / ("lib" + tool + ".so")).is_file())

    def test_all_platforms_without_system_property_helpers(self):
        self.fixture()
        for abi in ("arm64-v8a", "armeabi-v7a", "x86_64", "x86"):
            with self.subTest(abi=abi):
                result = self.run_adapter(abi=abi)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_or_invalid_platform_rejected_before_entrypoint(self):
        self.fixture()
        for abi, api in (("", "37"), ("unknown", "37"), ("arm64-v8a", ""),
                         ("arm64-v8a", "bad"), ("arm64-v8a", "22")):
            with self.subTest(abi=abi, api=api):
                result = self.run_adapter(abi=abi, api=api)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.work / "reached").exists())

    def test_upstream_failure_is_propagated(self):
        self.fixture()
        result = self.run_adapter("42")
        self.assertEqual(result.returncode, 42, result.stderr)

    def test_changed_installer_is_rejected_before_entrypoint(self):
        self.fixture(ENTRYPOINT.replace(MOUNT, ":"))
        result = self.run_adapter()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsupported Magisk installer layout", result.stderr)
        self.assertFalse((self.work / "reached").exists())

    def test_corrupt_archive_is_rejected(self):
        self.package.write_bytes(b"not a zip")
        result = self.run_adapter()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.work / "reached").exists())

if __name__ == "__main__":
    unittest.main(verbosity=2)
