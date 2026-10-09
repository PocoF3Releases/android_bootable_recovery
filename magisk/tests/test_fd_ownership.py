#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise actual FD conversion with libbase's Bionic tagging enabled on host.

The fdopen wrapper models Bionic's documented zero-owner requirement. This is
not a phone test. No partition, Android runtime or recovery build output access.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CHECKOUT = ROOT.parents[1]

HARNESS = r"""
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>
#define __BIONIC__ 1
#define __BIONIC_AVAILABILITY_GUARD(api) 1
#define __INTRODUCED_IN(api)
#define _Nonnull
#include <android-base/unique_fd.h>

static std::unordered_map<int, uint64_t> tags;
extern "C" uint64_t android_fdsan_create_owner_tag(android_fdsan_owner_type type, uint64_t value) {
  return value ? (uint64_t(type) << 56) | (value & ((uint64_t(1) << 56) - 1)) : 0;
}
extern "C" void android_fdsan_exchange_owner_tag(int fd, uint64_t expected, uint64_t next) {
  assert(tags[fd] == expected);
  tags[fd] = next;
}
extern "C" int android_fdsan_close_with_tag(int fd, uint64_t tag) {
  assert(tags[fd] == tag);
  tags.erase(fd);
  return close(fd);
}
extern "C" FILE* __real_fdopen(int, const char*);
extern "C" FILE* __wrap_fdopen(int fd, const char* mode) {
  if (tags[fd] != 0) _exit(86);  // Bionic __FILE_init rejects owned descriptors.
  return __real_fdopen(fd, mode);
}

int main() {
  // Prove the old .get() pattern violates the modeled Bionic contract.
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    android::base::unique_fd reader(dup(STDIN_FILENO));
    fdopen(reader.get(), "r");
    _exit(87);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 86);

  // Compile and execute the exact current production conversion statement.
  {
    int fds[2];
    assert(pipe(fds) == 0);
    android::base::unique_fd reader(fds[0]), writer(fds[1]);
    assert(write(writer.get(), "ui_print ready\n", 15) == 15);
    writer.reset();
    PRODUCTION_CONVERSION
    assert(stream && reader.get() == -1);
    char line[64];
    assert(fgets(line, sizeof(line), stream.get()));
    assert(std::string(line) == "ui_print ready\n");
  }
  // The libbase helper must close an unowned FD even when fdopen fails.
  android::base::unique_fd failure(dup(STDIN_FILENO));
  int raw = failure.get();
  assert(android::base::Fdopen(std::move(failure), "invalid") == nullptr);
  assert(failure.get() == -1);
  assert(fcntl(raw, F_GETFD) == -1 && errno == EBADF);
}
"""

class OwnershipTest(unittest.TestCase):
    def test_production_conversion(self):
        source = (ROOT / "magisk_install.cpp").read_text()
        expression = re.search(r'std::unique_ptr<FILE, decltype\(&fclose\)> stream\(.*?;', source, re.S)
        self.assertIsNotNone(expression)
        with tempfile.TemporaryDirectory(prefix="evox-fdsan-", dir="/tmp") as temp:
            temp = Path(temp)
            include = temp / "android"
            include.mkdir()
            (include / "fdsan.h").symlink_to(CHECKOUT / "bionic/libc/include/android/fdsan.h")
            harness = temp / "ownership.cpp"
            harness.write_text(HARNESS.replace("PRODUCTION_CONVERSION", expression.group()))
            binary = temp / "ownership"
            subprocess.run(["clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(temp), "-I" + str(CHECKOUT / "system/libbase/include"),
                            str(harness), "-Wl,--wrap=fdopen", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

if __name__ == "__main__":
    unittest.main(verbosity=2)
