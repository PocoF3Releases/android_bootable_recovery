// SPDX-License-Identifier: Apache-2.0
#pragma once

class RecoveryUI;

// Installs only the checksum-pinned package included in the recovery ramdisk.
bool InstallBundledMagisk(RecoveryUI* ui);

bool InstallBundledAPatch(RecoveryUI* ui);
