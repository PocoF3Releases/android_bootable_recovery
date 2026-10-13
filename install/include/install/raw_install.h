/* Copyright (C) 2026 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <string>
#include "install/install.h"
InstallResult InstallRawImage(const std::string& path, Device* device);
