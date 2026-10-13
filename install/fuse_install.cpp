/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "install/fuse_install.h"

#include <dirent.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <android-base/logging.h>
#include <android-base/scopeguard.h>
#include <android-base/strings.h>

#include "bootloader_message/bootloader_message.h"
#include "fuse_provider.h"
#include "fuse_sideload.h"
#include "install/install.h"
#include "install/raw_install.h"
#include "install/snapshot_utils.h"
#include "recovery_utils/roots.h"

namespace fs = std::filesystem;

using android::volmgr::VolumeInfo;
using android::volmgr::VolumeManager;

// How long (in seconds) we wait for the fuse-provided package file to
// appear, before timing out.
static constexpr int SDCARD_INSTALL_TIMEOUT = 10;

// Set the BCB to reboot back into recovery (it won't resume the install from
// sdcard though).
static void SetSdcardUpdateBootloaderMessage() {
  std::vector<std::string> options;
  std::string err;
  if (!update_bootloader_message(options, &err)) {
    LOG(ERROR) << "Failed to set BCB message: " << err;
  }
}

// Returns the selected filename, or an empty string.
std::string BrowseDirectory(const std::string& path, Device* device, RecoveryUI* ui) {
  if (access(path.c_str(), R_OK | X_OK)) {
    PLOG(ERROR) << "error opening " << path;
    return "";
  }

  std::vector<std::string> dirs;
  std::vector<std::string> entries{ "../" };  // "../" is always the first entry.

  std::error_code error;
  fs::directory_iterator iterator(path, error), end;
  for (; !error && iterator != end; iterator.increment(error)) {
    const auto& entry = *iterator;
    std::string name = entry.path().filename().string();

    // Skip "." and ".." entries.
    if (name == "." || name == "..") continue;

    if (entry.is_directory(error)) {
      dirs.push_back(name + "/");
    } else if (!error && entry.is_regular_file(error)) {
      if (android::base::EndsWithIgnoreCase(name, ".zip") ||
          android::base::EndsWithIgnoreCase(name, ".map") ||
          android::base::EndsWithIgnoreCase(name, ".img")) {
        entries.push_back(name);
      }
    }
  }

  if (error) {
    ui->Print("Unable to read storage: %s\n", error.message().c_str());
    return "";
  }

  std::sort(dirs.begin(), dirs.end());
  std::sort(entries.begin(), entries.end());

  // Append dirs to the entries list.
  entries.insert(entries.end(), dirs.begin(), dirs.end());

  std::vector<std::string> headers{ "Choose a ZIP or image to install:", path };

  size_t chosen_item = 0;
  while (true) {
    chosen_item = ui->ShowMenu(
        headers, entries, chosen_item, true,
        std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));

    // Return if WaitKey() was interrupted.
    if (chosen_item == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) {
      return "";
    }
    if (chosen_item == Device::kGoHome) {
      return "@";
    }
    if (chosen_item == Device::kGoBack || chosen_item == 0) {
      // Go up but continue browsing (if the caller is browse_directory).
      return "";
    }

    if (chosen_item >= entries.size()) return "";
    const std::string& item = entries[chosen_item];

    std::string new_path = path + "/" + item;
    if (new_path.back() == '/') {
      // Recurse down into a subdirectory.
      new_path.pop_back();
      std::string result = BrowseDirectory(new_path, device, ui);
      if (!result.empty()) return result;
    } else {
      // Selected a package or image: return the path to the caller.
      return new_path;
    }
  }

  // Unreachable.
}

static bool StartInstallPackageFuse(std::string_view path) {
  if (path.empty()) {
    return false;
  }

  constexpr auto FUSE_BLOCK_SIZE = 65536;
  bool is_block_map = android::base::ConsumePrefix(&path, "@");
  auto fuse_data_provider =
      is_block_map ? FuseBlockDataProvider::CreateFromBlockMap(std::string(path), FUSE_BLOCK_SIZE)
                   : FuseFileDataProvider::CreateFromFile(std::string(path), FUSE_BLOCK_SIZE);

  if (!fuse_data_provider || !fuse_data_provider->Valid()) {
    LOG(ERROR) << "Failed to create fuse data provider.";
    return false;
  }

  return run_fuse_sideload(std::move(fuse_data_provider)) == 0;
}

InstallResult InstallWithFuseFromPath(std::string_view path, Device* device) {
  // We used to use fuse in a thread as opposed to a process. Since accessing
  // through fuse involves going from kernel to userspace to kernel, it leads
  // to deadlock when a page fault occurs. (Bug: 26313124)
  auto ui = device->GetUI();
  pid_t child;
  if ((child = fork()) == 0) {
    bool status = StartInstallPackageFuse(path);

    _exit(status ? EXIT_SUCCESS : EXIT_FAILURE);
  }

  // FUSE_SIDELOAD_HOST_PATHNAME will start to exist once the fuse in child process is ready.
  InstallResult result = INSTALL_ERROR;
  int status;
  bool waited = false;
  for (int i = 0; i < SDCARD_INSTALL_TIMEOUT; ++i) {
    if (waitpid(child, &status, WNOHANG) == -1) {
      result = INSTALL_ERROR;
      waited = true;
      break;
    }

    struct stat sb;
    if (stat(FUSE_SIDELOAD_HOST_PATHNAME, &sb) == -1) {
      if (errno == ENOENT && i < SDCARD_INSTALL_TIMEOUT - 1) {
        sleep(1);
        continue;
      } else {
        LOG(ERROR) << "Timed out waiting for the fuse-provided package.";
        result = INSTALL_ERROR;
        kill(child, SIGKILL);
        break;
      }
    }
    auto package =
        Package::CreateFilePackage(FUSE_SIDELOAD_HOST_PATHNAME,
                                   std::bind(&RecoveryUI::SetProgress, ui, std::placeholders::_1));
    result = InstallPackage(package.get(), FUSE_SIDELOAD_HOST_PATHNAME, false, 0 /* retry_count */,
                            device);
    break;
  }

  if (!waited) {
    // Calling stat() on this magic filename signals the fuse
    // filesystem to shut down.
    struct stat sb;
    stat(FUSE_SIDELOAD_HOST_EXIT_PATHNAME, &sb);

    waitpid(child, &status, 0);
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    LOG(ERROR) << "Error exit from the fuse process: " << WEXITSTATUS(status);
  }

  return result;
}

InstallResult ApplyFromStorage(Device* device, VolumeInfo& vi) {
  auto ui = device->GetUI();
  const auto* data_volume = volume_for_mount_point("/data");
  const bool needs_crypto = vi.mId == "emulated" && data_volume &&
      !data_volume->metadata_encryption_options.empty();
  const auto release_dependencies = [&]() {
    bool success = true;
    if (needs_crypto) {
      // Child mounts must be released before their parent. Installed filesystem
      // keys remain in the kernel; browsing no longer needs the crypto HALs.
      for (const char* mount : {"/vendor/firmware_mnt", "/mnt/vendor/persist", "/odm", "/vendor"}) {
        if (volume_for_mount_point(mount) && ensure_path_unmounted(mount) != 0) {
          LOG(ERROR) << "Unable to release storage dependency: " << mount;
          success = false;
        }
      }
    }
    return success;
  };
  auto dependency_cleanup = android::base::make_scope_guard(release_dependencies);
  if (needs_crypto) {
    if (!CreateSnapshotPartitions()) {
      ui->Print("Unable to prepare encrypted storage.\n");
      return INSTALL_ERROR;
    }
    map_logical_partitions();
    // Snapshot preparation owns a temporary metadata mount and releases it.
    // Mount it afterward so the storage helper can read the existing keys.
    if (ensure_path_mounted("/metadata") != 0) {
      ui->Print("Unable to mount encryption metadata.\n");
      return INSTALL_ERROR;
    }
    for (const char* mount : {"/vendor", "/odm", "/vendor/firmware_mnt", "/mnt/vendor/persist"}) {
      if (volume_for_mount_point(mount) && ensure_path_mounted(mount) != 0) {
        ui->Print("Unable to mount storage dependency: %s\n", mount);
        return INSTALL_ERROR;
      }
    }
  }
  const bool mounted = VolumeManager::Instance()->volumeMount(vi.mId);
  const bool released = release_dependencies();
  dependency_cleanup.Disable();
  if (!mounted || !released) {
    if (mounted) VolumeManager::Instance()->volumeUnmount(vi.mId);
    ui->Print("Storage is locked or unavailable.\n");
    return INSTALL_NONE;
  }

  std::string path = BrowseDirectory(vi.mPath, device, ui);
  if (path.empty() || path == "@") {
    VolumeManager::Instance()->volumeUnmount(vi.mId);
    return INSTALL_NONE;
  }

  // Hint the install function to read from a block map file.
  if (android::base::EndsWithIgnoreCase(path, ".map")) {
    path = "@" + path;
  }

  ui->Print("\n-- Install %s ...\n", path.c_str());
  SetSdcardUpdateBootloaderMessage();

  auto result = android::base::EndsWithIgnoreCase(path, ".img")
      ? InstallRawImage(path, device) : InstallWithFuseFromPath(path, device);

  VolumeManager::Instance()->volumeUnmount(vi.mId);
  return result;
}
