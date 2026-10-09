#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the actual Magisk menu action against checkout headers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CHECKOUT = ROOT.parents[1]

class MagiskActionTest(unittest.TestCase):
    def test_magisk_action_against_real_checkout_headers(self):
        source = (ROOT / "recovery.cpp").read_text()
        actions = source[source.index("      case Device::INSTALL_MAGISK:"):
                         source.index("      case Device::SWAP_SLOT:")]
        fixture = """#include <chrono>
#include <functional>
#include <string>
#include <vector>
// Expose the real platform-only property declarations for this syntax check.
#define __BIONIC__ 1
#include <android-base/properties.h>
#undef __BIONIC__
#include "install/install.h"
#include "install/snapshot_utils.h"
#include "otautil/paths.h"
#include "recovery_ui/device.h"
#include "recovery_ui/ui.h"
#include "magisk_install.h"
bool yes_no(Device*, const char*, const char*);
void copy_logs(bool);
void CheckActions(Device* device, Device::BuiltinAction chosen_action, InstallResult status) {
  auto ui = device->GetUI();
  bool update_in_progress = false, reboot_before_magisk = false, save_current_log = false;
  switch (chosen_action) {
""" + actions + """    default: break;
  }
}
"""
        includes = [ROOT, ROOT / "recovery_ui/include", ROOT / "install/include",
                    ROOT / "recovery_utils/include", ROOT / "otautil/include",
                    CHECKOUT / "system/libbase/include", CHECKOUT / "system/libziparchive/include",
                    CHECKOUT / "system/fs/fs_mgr/libfstab/include",
                    CHECKOUT / "external/boringssl/src/include"]
        with tempfile.TemporaryDirectory(prefix="recovery-action-check-", dir="/tmp") as temp:
            file = Path(temp) / "actions.cpp"
            file.write_text(fixture)
            subprocess.run(["clang++", "-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
                            "-D__printflike(a,b)=__attribute__((format(printf,a,b)))",
                            *["-I" + str(path) for path in includes], str(file)], check=True)

if __name__ == "__main__":
    unittest.main(verbosity=2)
