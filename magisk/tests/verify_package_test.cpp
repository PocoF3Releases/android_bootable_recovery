// SPDX-License-Identifier: Apache-2.0
// Include the production checksum function. Unused Android runtime sections are
// discarded by the host link; no recovery or partition operations are executed.
#include <fstream>
#include <iterator>
#include "../../magisk_install.cpp"

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  std::ifstream file(argv[1], std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(file)), {});
  if (!file || !VerifyPackage(content)) return 2;
  content[content.size() / 2] ^= 1;
  if (VerifyPackage(content)) return 3;
  content.resize(content.size() / 2);
  if (VerifyPackage(content) || VerifyPackage("")) return 4;
  return 0;
}
