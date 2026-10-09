// SPDX-License-Identifier: Apache-2.0
#include "magisk_install.h"

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
constexpr char kPackage[] = "/system/etc/recovery/magisk.apk";
constexpr char kAdapter[] = "/system/etc/recovery/install-magisk.sh";
constexpr char kSha256[] = "2c8a488b9a5293e578e95ae4f07e3c57aba4feec4a52ca4dd852a2692d6dd4e8";

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
  char path_[sizeof("/tmp/evox-magisk-XXXXXX")] = "/tmp/evox-magisk-XXXXXX";
  bool valid_;
};

bool VerifyPackage(const std::string& content) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest;
  SHA256(reinterpret_cast<const unsigned char*>(content.data()), content.size(), digest.data());
  constexpr char hex[] = "0123456789abcdef";
  std::string actual;
  for (unsigned char c : digest) {
    actual += hex[c >> 4];
    actual += hex[c & 15];
  }
  return actual == kSha256;
}

bool ExtractBusybox(const std::string& package, const std::string& path) {
  ZipArchiveHandle archive;
  if (OpenArchive(package.c_str(), &archive) != 0) return false;
  ZipEntry64 entry;
  bool ok = false;
  if (FindEntry(archive, kBusyboxEntry, &entry) == 0) {
    android::base::unique_fd fd(open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0700));
    ok = fd.ok() && ExtractEntryToFile(archive, &entry, fd.get()) == 0;
  }
  CloseArchive(archive);
  return ok;
}

bool RunInstaller(RecoveryUI* ui, const std::string& work) {
  android::base::unique_fd reader, writer;
  // The updater protocol descriptor must survive exec. All other staging FDs are closed.
  if (!android::base::Pipe(&reader, &writer, 0)) return false;
  const std::string busybox = work + "/busybox";
  const std::string script = work + "/install-magisk.sh";
  const std::string package = work + "/magisk.apk";
  const std::string outfd = std::to_string(writer.get());
  // Snapshot platform metadata while recovery's toolbox/libraries are still
  // visible. Magisk mounts Android over /system only in the child namespace.
  int sdk = android::base::GetIntProperty<int>("ro.build.version.sdk", 0);
  if (sdk < 23 || kAbi[0] == '\0') {
    ui->Print("Cannot detect a supported recovery platform (SDK %d, ABI %s).\n", sdk, kAbi);
    return false;
  }
  const std::string api = std::to_string(sdk);
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
        LOG(INFO) << "Magisk: " << command;
      }
    }
    free(line);
    read_ok = !ferror(stream.get());
  }
  stream.reset();
  int status = 0;
  pid_t waited = TEMP_FAILURE_RETRY(waitpid(child, &status, 0));
  if (waited != child) {
    ui->Print("Could not wait for Magisk installer: %s\n", strerror(errno));
    return false;
  }
  if (WIFSIGNALED(status)) {
    ui->Print("Magisk installer terminated by signal %d.\n", WTERMSIG(status));
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !read_ok) {
    if (WIFEXITED(status))
      ui->Print("Magisk installer exited with status %d.\n", WEXITSTATUS(status));
    if (!read_ok) ui->Print("Could not read Magisk installer output.\n");
    LOG(ERROR) << "Bundled Magisk installer failed";
    return false;
  }
  sync();
  return true;
}
}  // namespace

bool InstallBundledMagisk(RecoveryUI* ui) {
  ui->ShowText(true);
  RamWorkspace ram;
  if (!ram.valid()) {
    ui->Print("Cannot create Magisk workspace in recovery RAM.\n");
    return false;
  }
  // Copy before Magisk mounts Android over /system in its own namespace.
  std::string content;
  if (!android::base::ReadFileToString(kPackage, &content) || !VerifyPackage(content)) {
    ui->Print("Bundled Magisk is missing or failed checksum verification.\n");
    return false;
  }
  const std::string package = ram.path() + "/magisk.apk";
  std::string adapter;
  if (!android::base::WriteStringToFile(content, package) ||
      !android::base::ReadFileToString(kAdapter, &adapter) ||
      !android::base::WriteStringToFile(adapter, ram.path() + "/install-magisk.sh") ||
      !ExtractBusybox(package, ram.path() + "/busybox")) {
    ui->Print("Cannot prepare bundled Magisk in recovery RAM.\n");
    return false;
  }
  // Magisk's upstream installer needs the logical system partition for detection.
  if (!logical_partitions_mapped()) {
    if (!CreateSnapshotPartitions()) {
      ui->Print("Cannot prepare system partitions for Magisk.\n");
      return false;
    }
    map_logical_partitions();
  }
  ui->Print("Installing bundled Magisk v31.0...\n");
  ui->SetBackground(RecoveryUI::INSTALLING_UPDATE);
  ui->SetProgressType(RecoveryUI::INDETERMINATE);
  ui->SetEnableReboot(false);
  bool success = RunInstaller(ui, ram.path());
  ui->SetEnableReboot(true);
  ui->SetProgressType(RecoveryUI::EMPTY);
  ui->SetBackground(success ? RecoveryUI::NO_COMMAND : RecoveryUI::ERROR);
  ui->Print(success ? "Magisk installation completed. Reboot system to use root.\n"
                    : "Magisk installation failed. See recovery logs for details.\n");
  return success;
}
