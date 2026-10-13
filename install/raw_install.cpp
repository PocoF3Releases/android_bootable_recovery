/*
 * Copyright (C) 2026 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "install/raw_install.h"

#include <fcntl.h>
#include <endian.h>
#include <bootimg.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>

#include "recovery_ui/device.h"
#include "recovery_ui/ui.h"

namespace {

template <typename Header>
bool ReadHeader(int fd, uint64_t size, Header* header) {
  return size >= sizeof(Header) &&
         android::base::ReadFullyAtOffset(fd, header, sizeof(Header), 0);
}

bool IsPageSize(uint32_t page) {
  return page >= 2048 && page <= 65536 && (page & (page - 1)) == 0;
}

uint64_t Rounded(uint64_t size, uint32_t page) {
  return (size + page - 1) & ~(static_cast<uint64_t>(page) - 1);
}

std::vector<std::string> ImageDestinations(int fd, uint64_t size,
                                          const std::array<char, 8>& magic) {
  if (memcmp(magic.data(), BOOT_MAGIC, BOOT_MAGIC_SIZE) == 0) {
    boot_img_hdr_v0 header {};
    if (!ReadHeader(fd, size, &header)) return {};
    const auto version = le32toh(header.header_version);
    if (version <= 2) {
      const auto page = le32toh(header.page_size);
      if (!IsPageSize(page) || sizeof(header) > page || header.kernel_size == 0) return {};
      uint64_t required = page + Rounded(le32toh(header.kernel_size), page) +
          Rounded(le32toh(header.ramdisk_size), page) +
          Rounded(le32toh(header.second_size), page);
      if (version >= 1) {
        boot_img_hdr_v1 v1 {};
        const auto expected = version == 1 ? sizeof(v1) : sizeof(boot_img_hdr_v2);
        if (!ReadHeader(fd, size, &v1) || le32toh(v1.header_size) != expected ||
            expected > page) return {};
        if (v1.recovery_dtbo_size != 0 && le64toh(v1.recovery_dtbo_offset) != required) return {};
        required += Rounded(le32toh(v1.recovery_dtbo_size), page);
      }
      if (version == 2) {
        boot_img_hdr_v2 v2 {};
        if (!ReadHeader(fd, size, &v2)) return {};
        required += Rounded(le32toh(v2.dtb_size), page);
      }
      return required <= size ? std::vector<std::string>{"boot", "recovery"}
                              : std::vector<std::string>{};
    }
    if (version == 3 || version == 4) {
      boot_img_hdr_v4 v4 {};
      if (!ReadHeader(fd, size, &v4)) return {};
      const auto expected = version == 3 ? sizeof(boot_img_hdr_v3) : sizeof(v4);
      if (le32toh(v4.header_size) != expected) return {};
      uint64_t required = 4096 + Rounded(le32toh(v4.kernel_size), 4096) +
          Rounded(le32toh(v4.ramdisk_size), 4096);
      if (version == 4) required += Rounded(le32toh(v4.signature_size), 4096);
      if (required > size || (v4.kernel_size == 0 && v4.ramdisk_size == 0)) return {};
      if (v4.kernel_size == 0) return version == 4 ? std::vector<std::string>{"init_boot"}
                                                                : std::vector<std::string>{};
      return {"boot", "recovery"};
    }
    return {};
  }
  if (memcmp(magic.data(), VENDOR_BOOT_MAGIC, VENDOR_BOOT_MAGIC_SIZE) == 0) {
    vendor_boot_img_hdr_v4 header {};
    if (!ReadHeader(fd, size, &header)) return {};
    const auto version = le32toh(header.header_version);
    const auto page = le32toh(header.page_size);
    if ((version != 3 && version != 4) || !IsPageSize(page)) return {};
    const auto expected = version == 3 ? sizeof(vendor_boot_img_hdr_v3) : sizeof(header);
    if (le32toh(header.header_size) != expected || expected > page) return {};
    uint64_t required = page + Rounded(le32toh(header.vendor_ramdisk_size), page) +
        Rounded(le32toh(header.dtb_size), page);
    if (version == 4) {
      if (static_cast<uint64_t>(le32toh(header.vendor_ramdisk_table_entry_num)) *
          le32toh(header.vendor_ramdisk_table_entry_size) >
          le32toh(header.vendor_ramdisk_table_size)) return {};
      required += Rounded(le32toh(header.vendor_ramdisk_table_size), page) +
          Rounded(le32toh(header.bootconfig_size), page);
    }
    return required <= size ? std::vector<std::string>{"vendor_boot"}
                            : std::vector<std::string>{};
  }
  if (memcmp(magic.data(), "AVB0", 4) == 0) {
    std::array<uint8_t, 256> header {};
    if (!ReadHeader(fd, size, &header)) return {};
    uint64_t auth, aux;
    memcpy(&auth, header.data() + 12, sizeof(auth));
    memcpy(&aux, header.data() + 20, sizeof(aux));
    auth = be64toh(auth); aux = be64toh(aux);
    const auto remaining = size - header.size();
    if (auth > remaining || aux > remaining - auth) return {};
    return {"vbmeta", "vbmeta_system"};
  }
  if (memcmp(magic.data(), "\xd7\xb7\xab\x1e", 4) == 0) {
    std::array<uint32_t, 8> header {};
    if (!ReadHeader(fd, size, &header)) return {};
    const auto total = be32toh(header[1]);
    const auto entry_size = be32toh(header[3]);
    const auto count = be32toh(header[4]);
    const auto start = be32toh(header[5]);
    if (total > size || be32toh(header[2]) != sizeof(header) || entry_size < 32 ||
        count == 0 || start < sizeof(header) || start > total ||
        static_cast<uint64_t>(count) * entry_size > total - start) return {};
    for (uint32_t i = 0; i < count; ++i) {
      std::array<uint32_t, 2> entry {};
      if (!android::base::ReadFullyAtOffset(fd, entry.data(), sizeof(entry),
          start + static_cast<uint64_t>(i) * entry_size)) return {};
      const auto offset = be32toh(entry[1]);
      if (offset > total || be32toh(entry[0]) > total - offset) return {};
    }
    return {"dtbo"};
  }
  return {};
}

}  // namespace

InstallResult InstallRawImage(const std::string& path, Device* device) {
  auto ui = device->GetUI();
  android::base::unique_fd input(open(path.c_str(), O_RDONLY | O_CLOEXEC));
  struct stat image_stat {};
  std::array<char, 8> magic {};
  if (input < 0 || fstat(input.get(), &image_stat) != 0 ||
      !S_ISREG(image_stat.st_mode) || image_stat.st_size <= 0 ||
      !android::base::ReadFullyAtOffset(input.get(), magic.data(), magic.size(), 0)) {
    ui->Print("Unable to read image.\n");
    return INSTALL_ERROR;
  }

  // Only boot-chain images are written here. Fastbootd owns filesystem images,
  // logical partitions and snapshot-aware flashing.
  const auto bases = ImageDestinations(input.get(), image_stat.st_size, magic);
  if (bases.empty()) {
    ui->Print("Unsupported or truncated image. Use Advanced > Enter fastboot for\n"
              "filesystem, sparse or logical partition images.\n");
    return INSTALL_CORRUPT;
  }

  std::vector<std::string> partitions;
  std::vector<std::string> items {"Cancel"};
  const auto current_slot = android::base::GetProperty("ro.boot.slot_suffix", "");
  for (const auto& base : bases) {
    const std::vector<std::string> suffixes = current_slot.empty()
        ? std::vector<std::string>{""} : std::vector<std::string>{"_a", "_b"};
    for (const auto& suffix : suffixes) {
      const auto name = base + suffix;
      struct stat target_stat {};
      if (stat(("/dev/block/by-name/" + name).c_str(), &target_stat) != 0 ||
          !S_ISBLK(target_stat.st_mode)) continue;
      partitions.push_back(name);
      items.push_back(name + ((!suffix.empty() && suffix == current_slot)
          ? " (current slot)" : ""));
    }
  }
  if (partitions.empty()) {
    ui->Print("No compatible boot-chain partition found.\n");
    return INSTALL_ERROR;
  }
  const auto handler = std::bind(&Device::HandleMenuKey, device, std::placeholders::_1,
                                 std::placeholders::_2);
  const auto choice = ui->ShowMenu({"Select image destination:", path}, items, 0, true, handler);
  if (choice <= 0 || static_cast<size_t>(choice) >= items.size()) return INSTALL_NONE;
  const auto name = partitions[choice - 1];
  const auto target = "/dev/block/by-name/" + name;
  android::base::unique_fd output(open(target.c_str(), O_RDONLY | O_CLOEXEC));
  struct stat target_stat {};
  uint64_t capacity = 0;
  if (output < 0 || fstat(output.get(), &target_stat) != 0 || !S_ISBLK(target_stat.st_mode) ||
      ioctl(output.get(), BLKGETSIZE64, &capacity) != 0 ||
      static_cast<uint64_t>(image_stat.st_size) > capacity) {
    ui->Print("Image does not fit or destination is unavailable.\n");
    return INSTALL_ERROR;
  }
  if (ui->ShowMenu({"Overwrite " + name + "?", path}, {"Cancel", "Flash image"},
                   0, true, handler) != 1) return INSTALL_NONE;
  output.reset(open(target.c_str(), O_RDWR | O_CLOEXEC));
  struct stat opened_stat {};
  if (output < 0 || fstat(output.get(), &opened_stat) != 0 ||
      !S_ISBLK(opened_stat.st_mode) || opened_stat.st_rdev != target_stat.st_rdev) {
    ui->Print("Destination changed or is not writable.\n");
    return INSTALL_ERROR;
  }

  ui->SetEnableReboot(false);
  ui->SetProgressType(RecoveryUI::DETERMINATE);
  ui->ShowProgress(1.0, 0);
  std::vector<char> buffer(1024 * 1024), verify(buffer.size());
  bool success = true;
  for (off_t offset = 0; offset < image_stat.st_size; offset += buffer.size()) {
    const auto count = std::min<off_t>(buffer.size(), image_stat.st_size - offset);
    if (!android::base::ReadFullyAtOffset(input.get(), buffer.data(), count, offset) ||
        !android::base::WriteFully(output.get(), buffer.data(), count)) {
      success = false;
      break;
    }
    ui->SetProgress(0.5 * (offset + count) / image_stat.st_size);
  }
  if (success && fsync(output.get()) != 0) success = false;
  for (off_t offset = 0; success && offset < image_stat.st_size; offset += buffer.size()) {
    const auto count = std::min<off_t>(buffer.size(), image_stat.st_size - offset);
    success = android::base::ReadFullyAtOffset(input.get(), buffer.data(), count, offset) &&
              android::base::ReadFullyAtOffset(output.get(), verify.data(), count, offset) &&
              memcmp(buffer.data(), verify.data(), count) == 0;
    ui->SetProgress(0.5 + 0.5 * (offset + count) / image_stat.st_size);
  }
  ui->SetEnableReboot(true);
  ui->Print(success ? "Image flashed and verified.\n"
                    : "Image write or read-back verification failed.\n");
  return success ? INSTALL_SUCCESS : INSTALL_ERROR;
}
