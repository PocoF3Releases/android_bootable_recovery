#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the actual Enable ADB action with deterministic property/UI test doubles."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CHECKOUT = ROOT.parents[1]
HARNESS = r"""
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
struct UI {
  bool failure = false;
  void Print(const char* text) { failure = std::string(text).find("Could not") == 0; }
};
struct Device {
  enum Action { ENABLE_ADB };
  bool removed = false, home = false;
  void RemoveMenuItemForAction(Action) { removed = true; }
  void GoHome() { home = true; }
};
namespace android::base {
  int scenario, waits;
  bool SetProperty(const std::string& name, const std::string& value) {
    assert(name == "sys.usb.config.recovery_adb" && value == "requested");
    return scenario != 1;
  }
  bool WaitForProperty(const std::string& name, const std::string& value, std::chrono::milliseconds) {
    ++waits;
    if (waits == 1) {
      assert(name == "sys.usb.config.recovery_adb" && value == "enabled");
      return scenario != 2;
    }
    assert(name == "ro.adb.secure.recovery" && value == "0");
    return scenario != 3;
  }
}
void RunAction(Device* device, UI* ui) {
  do { ACTION } while (false);
}
int main() {
  for (int scenario = 0; scenario < 4; ++scenario) {
    android::base::scenario = scenario;
    android::base::waits = 0;
    Device device;
    UI ui;
    RunAction(&device, &ui);
    assert(device.removed == (scenario == 0));
    assert(device.home == (scenario == 0));
    assert(ui.failure == (scenario != 0));
  }
}
"""

class AdbActionTest(unittest.TestCase):
    def test_actual_menu_action_success_and_failure(self):
        source = (ROOT / "recovery.cpp").read_text()
        action = re.search(r'case Device::ENABLE_ADB:\n(.*?)\n      case Device::',
                           source, re.S).group(1)
        with tempfile.TemporaryDirectory(prefix="recovery-adb-check-", dir="/tmp") as temp:
            temp = Path(temp)
            harness = temp / "adb.cpp"
            harness.write_text(HARNESS.replace("ACTION", action))
            binary = temp / "adb"
            subprocess.run(["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(harness), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_adb_action_against_real_checkout_headers(self):
        source = (ROOT / "recovery.cpp").read_text()
        actions = source[source.index("      case Device::ENABLE_ADB:"):
                         source.index("      case Device::", source.index("      case Device::ENABLE_ADB:") + 1)]
        fixture = """#include <chrono>
#include <functional>
#include <string>
#include <vector>
// Expose the real platform-only property declarations for this syntax check.
#define __BIONIC__ 1
#include <android-base/properties.h>
#undef __BIONIC__
#include "recovery_ui/device.h"
#include "recovery_ui/ui.h"
void CheckActions(Device* device, Device::BuiltinAction chosen_action) {
  auto ui = device->GetUI();
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

    def test_init_request_and_existing_property_permission(self):
        rc = (ROOT / "etc/init.rc").read_text()
        action = re.search(r'on property:sys.usb.config.recovery_adb=requested\n((?:    .*\n)+)', rc)
        self.assertIsNotNone(action)
        self.assertEqual(action.group(1).splitlines(), [
            "    setprop ro.adb.secure.recovery 0", "    restart adbd",
            "    setprop sys.usb.config.recovery_adb enabled"])
        policy = (CHECKOUT / "system/sepolicy/private/property_contexts").read_text()
        self.assertRegex(policy, r'sys\.usb\.config\.\s+u:object_r:usb_prop:s0')
        recovery = (CHECKOUT / "system/sepolicy/private/recovery.te").read_text()
        self.assertIn("set_prop(recovery, usb_prop)", recovery)

if __name__ == "__main__":
    unittest.main(verbosity=2)
