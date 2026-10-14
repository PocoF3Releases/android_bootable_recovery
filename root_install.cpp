// SPDX-License-Identifier: Apache-2.0
#include "root_install.h"

#include <fcntl.h>
#include <ftw.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <openssl/sha.h>
#include <ziparchive/zip_archive.h>

#include "install/snapshot_utils.h"
#include "recovery_ui/ui.h"
#include "recovery_utils/roots.h"

namespace {
struct InstallerSpec {
  const char* package;
  const char* adapter;
  const char* sha256;
  const char* label;
  bool apatch;
};
constexpr InstallerSpec kMagisk{ "/system/etc/recovery/magisk.apk",
                                 "/system/etc/recovery/install-magisk.sh",
                                 "2c8a488b9a5293e578e95ae4f07e3c57aba4feec4a52ca4dd852a2692d6dd4e8",
                                 "Magisk v31.0", false };
constexpr InstallerSpec kAPatch{ "/system/etc/recovery/apatch.apk",
                                 "/system/etc/recovery/install-apatch.sh",
                                 "f1986f9a9c3b2b2d5ca09aa6b9ef24226fef662f8faf5677ce1447ffcae68590",
                                 "APatch 11224", true };

#if defined(__aarch64__)
constexpr char kBusyboxEntry[] = "lib/arm64-v8a/libbusybox.so";
constexpr char kAbi[] = "arm64-v8a";
#elif defined(__arm__)
constexpr char kBusyboxEntry[] = "lib/armeabi-v7a/libbusybox.so";
constexpr char kAbi[] = "armeabi-v7a";
#elif defined(__x86_64__)
constexpr char kBusyboxEntry[] = "lib/x86_64/libbusybox.so";
constexpr char kAbi[] = "x86_64";
#elif defined(__i386__)
constexpr char kBusyboxEntry[] = "lib/x86/libbusybox.so";
constexpr char kAbi[] = "x86";
#else
constexpr char kBusyboxEntry[] = "";
constexpr char kAbi[] = "";
#endif

// Explicitly use recovery RAM, never libbase's Android default (/data/local/tmp).
class RamWorkspace {
 public:
  RamWorkspace() {
    valid_ = mkdtemp(path_) != nullptr;
  }
  ~RamWorkspace() {
    if (!valid_) return;
    nftw(
        path_,
        [](const char* path, const struct stat*, int type, struct FTW*) {
          return type == FTW_DP ? rmdir(path) : unlink(path);
        },
        32, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
  }
  bool valid() const {
    return valid_;
  }
  std::string path() const {
    return path_;
  }

 private:
  char path_[sizeof("/tmp/evox-root-XXXXXX")] = "/tmp/evox-root-XXXXXX";
  bool valid_;
};

bool VerifyPackage(const std::string& content, const char* expected) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest;
  SHA256(reinterpret_cast<const unsigned char*>(content.data()), content.size(), digest.data());
  constexpr char hex[] = "0123456789abcdef";
  std::string actual;
  for (unsigned char c : digest) {
    actual += hex[c >> 4];
    actual += hex[c & 15];
  }
  return actual == expected;
}

bool ExtractTool(const std::string& package, const char* name, const std::string& path) {
  ZipArchiveHandle archive;
  if (OpenArchive(package.c_str(), &archive) != 0) return false;
  ZipEntry64 entry;
  bool ok = false;
  if (FindEntry(archive, name, &entry) == 0) {
    android::base::unique_fd fd(open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0700));
    ok = fd.ok() && ExtractEntryToFile(archive, &entry, fd.get()) == 0;
  }
  CloseArchive(archive);
  return ok;
}

bool RunInstaller(RecoveryUI* ui, const std::string& work, const InstallerSpec& spec) {
  android::base::unique_fd reader, writer;
  // The updater protocol descriptor must survive exec. All other staging FDs are closed.
  if (!android::base::Pipe(&reader, &writer, 0)) return false;
  const std::string busybox = work + "/busybox";
  const std::string script = work + "/install-root.sh";
  const std::string package = work + "/root.apk";
  const std::string outfd = std::to_string(writer.get());
  // Snapshot platform metadata while recovery's toolbox/libraries are still
  // visible. Magisk mounts Android over /system only in the child namespace.
  int sdk = android::base::GetIntProperty<int>("ro.build.version.sdk", 0);
  if (sdk < 23 || kAbi[0] == '\0') {
    ui->Print("Cannot detect a supported recovery platform (SDK %d, ABI %s).\n", sdk, kAbi);
    return false;
  }
  const std::string api = std::to_string(sdk);
  const std::string slot = android::base::GetProperty("ro.boot.slot_suffix", "");
  if (spec.apatch && slot != "" && slot != "_a" && slot != "_b") {
    ui->Print("Cannot determine the active boot slot.\n");
    return false;
  }
  const std::string boot = "/dev/block/by-name/boot" + slot;
  pid_t child = fork();
  if (child == -1) return false;
  if (child == 0) {
    close(reader.release());
    if (dup2(writer.get(), STDOUT_FILENO) == -1 || dup2(writer.get(), STDERR_FILENO) == -1) {
      _exit(126);
    }
    // Magisk mounts and unmounts /system, /vendor and /dev/random. Never propagate
    // those changes to the recovery UI or its dynamically linked services.
    if (unshare(CLONE_NEWNS) == -1 ||
        mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == -1) {
      dprintf(writer.get(), "ui_print ! Cannot isolate installer mounts: %s\n", strerror(errno));
      _exit(126);
    }
    if (setenv("EVO_MAGISK_API", api.c_str(), 1) == -1 || setenv("EVO_MAGISK_ABI", kAbi, 1) == -1 ||
        setenv("EVO_MAGISK_WORK", work.c_str(), 1) == -1 ||
        setenv("EVO_APATCH_BOOT", boot.c_str(), 1) == -1 ||
        setenv("BBBIN", busybox.c_str(), 1) == -1 || setenv("ASH_STANDALONE", "1", 1) == -1) {
      _exit(126);
    }
    execl(busybox.c_str(), busybox.c_str(), "sh", script.c_str(), "3", outfd.c_str(),
          package.c_str(), static_cast<char*>(nullptr));
    dprintf(writer.get(), "ui_print ! Cannot start Magisk installer: %s\n", strerror(errno));
    _exit(127);
  }

  writer.reset();
  // Transfer ownership before fdopen. Bionic's FILE constructor claims the FD
  // with fdsan and aborts if unique_fd still owns it. Fdopen also closes on failure.
  std::unique_ptr<FILE, decltype(&fclose)> stream(android::base::Fdopen(std::move(reader), "r"),
                                                  fclose);
  bool read_ok = stream != nullptr;
  if (stream) {
    char* line = nullptr;
    size_t size = 0;
    while (getline(&line, &size, stream.get()) != -1) {
      std::string command = android::base::Trim(line);
      if (command == "ui_print") {
        ui->Print("\n");
      } else if (android::base::StartsWith(command, "ui_print ")) {
        ui->Print("%s\n", command.substr(9).c_str());
      } else {
        // Archive extraction and binary diagnostics go to the recovery log.
        LOG(INFO) << spec.label << ": " << command;
      }
    }
    free(line);
    read_ok = !ferror(stream.get());
  }
  stream.reset();
  int status = 0;
  pid_t waited = TEMP_FAILURE_RETRY(waitpid(child, &status, 0));
  if (waited != child) {
    ui->Print("Could not wait for root installer: %s\n", strerror(errno));
    return false;
  }
  if (WIFSIGNALED(status)) {
    ui->Print("%s installer terminated by signal %d.\n", spec.label, WTERMSIG(status));
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !read_ok) {
    if (WIFEXITED(status))
      ui->Print("%s installer exited with status %d.\n", spec.label, WEXITSTATUS(status));
    if (!read_ok) ui->Print("Could not read root installer output.\n");
    LOG(ERROR) << spec.label << " installer failed";
    return false;
  }
  sync();
  return true;
}
bool InstallBundledRoot(RecoveryUI* ui, const InstallerSpec& spec) {
  if (spec.apatch && std::string(kAbi) != "arm64-v8a") {
    ui->Print("APatch requires an ARM64 recovery and kernel.\n");
    return false;
  }
  ui->ShowText(true);
  RamWorkspace ram;
  if (!ram.valid()) {
    ui->Print("Cannot create root installer workspace in recovery RAM.\n");
    return false;
  }
  // Copy before Magisk mounts Android over /system in its own namespace.
  std::string content;
  if (!android::base::ReadFileToString(spec.package, &content) ||
      !VerifyPackage(content, spec.sha256)) {
    ui->Print("Bundled root package is missing or failed checksum verification.\n");
    return false;
  }
  const std::string package = ram.path() + "/root.apk";
  std::string adapter;
  if (!android::base::WriteStringToFile(content, package) ||
      !android::base::ReadFileToString(spec.adapter, &adapter) ||
      !android::base::WriteStringToFile(adapter, ram.path() + "/install-root.sh") ||
      !ExtractTool(package, kBusyboxEntry, ram.path() + "/busybox")) {
    ui->Print("Cannot prepare bundled root installer in recovery RAM.\n");
    return false;
  }
  if (spec.apatch) {
    std::string magisk;
    if (!android::base::ReadFileToString(kMagisk.package, &magisk) ||
        !VerifyPackage(magisk, kMagisk.sha256) ||
        !android::base::WriteStringToFile(magisk, ram.path() + "/magisk-tools.apk") ||
        !ExtractTool(ram.path() + "/magisk-tools.apk", "lib/arm64-v8a/libmagiskboot.so",
                     ram.path() + "/magiskboot")) {
      ui->Print("Cannot prepare verified boot inspection tools.\n");
      return false;
    }
  }
  // Magisk's upstream installer needs the logical system partition for detection.
  if (!spec.apatch && !logical_partitions_mapped()) {
    if (!CreateSnapshotPartitions()) {
      ui->Print("Cannot prepare system partitions for Magisk.\n");
      return false;
    }
    map_logical_partitions();
  }
  ui->Print("Installing bundled %s...\n", spec.label);
  ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
  ui->SetProgressType(RecoveryUI::INDETERMINATE);
  ui->SetEnableReboot(false);
  bool success = RunInstaller(ui, ram.path(), spec);
  ui->SetEnableReboot(true);
  ui->SetProgressType(RecoveryUI::EMPTY);
  ui->SetBackground(success ? RecoveryUI::NO_COMMAND : RecoveryUI::ERROR);
  ui->Print(success ? "%s installation completed.\n"
                    : "%s installation failed. See recovery logs for details.\n",
            spec.label);
  return success;
}

}  // namespace

bool InstallBundledMagisk(RecoveryUI* ui) {
  return InstallBundledRoot(ui, kMagisk);
}

bool InstallBundledAPatch(RecoveryUI* ui) {
  return InstallBundledRoot(ui, kAPatch);
}
