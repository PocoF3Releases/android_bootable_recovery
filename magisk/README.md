# Bundled Magisk in EvoX recovery

Advanced → **Install Magisk v31.0 (bundled)** asks for confirmation, installs to
Magisk's detected boot/init_boot target for the booted slot, and returns to the
result screen, with an option to view the recovery log. Installation text stays
visible. Reboot recovery first after a successful update or selecting another slot.
Virtual A/B update safety is checked before the installer starts. Root is optional;
normal boot and sideload signature verification remain unchanged.

## Package and provenance

As verified on 2026-10-10, the newest official release is **v31.0**, a prerelease
with Android 17 support. GitHub's stable-only latest endpoint still selects v30.7.

- Release: https://github.com/topjohnwu/Magisk/releases/tag/v31.0
- Unmodified asset: https://github.com/topjohnwu/Magisk/releases/download/v31.0/Magisk-v31.0.apk
- Source and build instructions: https://github.com/topjohnwu/Magisk/tree/v31.0
- SHA256: `2c8a488b9a5293e578e95ae4f07e3c57aba4feec4a52ca4dd852a2692d6dd4e8`
- License: [GPL v3](LICENSE), declared separately from recovery's Apache license.

`recovery_bundled_magisk` and `recovery_bundled_magisk_script` are required by
`recovery`, installed to `/system/etc/recovery/` **inside the recovery ramdisk**.
They require no APK in internal storage, SD card, OTG, sideload or network access.
The native installer checks the pinned SHA256 before extracting executable code.

## Runtime adaptation

`magisk_install.cpp` copies the package and adapter to a unique `/tmp` directory,
extracts the official BusyBox matching the recovery executable architecture, and
starts it in a private mount namespace. Namespace setup failure aborts installation.
The pipe FD is transferred with libbase `Fdopen(std::move(reader), ...)` before
Bionic claims it; passing an owned `.get()` descriptor aborts under fdsan.
The parent receives installer output and exit status through the recovery API 3
pipe; extraction diagnostics remain in recovery logs. UI reboot keys are disabled
while the child runs. The parent cleans up its RAM workspace afterward.

The adapter keeps the APK bytes unchanged. It modifies only extracted scripts:
removes the initial Data/Cache mount attempt, overrides `check_data()` to select
RAM, disables reading storage configuration in `getvar()`, and skips persistent
stock-boot backups/module migration in `run_migrations()`. Boot patching, image
target detection and flashing use Magisk's upstream implementation. Default
verity/encryption preservation applies; existing Magisk modules on Data remain
untouched. This action creates no persistent stock-image backup and does not
claim to install the Android manager APK. Manage the app/modules after Android
boots. No decryption is needed for this recovery action.

This is an EvoX recovery change, separate from the paused PBRP work. No device
partition is written until the user confirms the recovery menu action.

## Updating and verification

Replace the official APK, update `kSha256`/menu/confirmation/status version,
prebuilt `src`, this provenance, and test checksum together. Inspect upstream
installer changes before updating the narrowly matched adapter. Never fetch a
moving release during the recovery build. Distribute the upstream source link,
build instructions, adapter source and GPL license with the binary.

Safe host tests (x86_64 Linux, Python 3):

```sh
python3 magisk/tests/test_adapter.py
python3 magisk/tests/test_fd_ownership.py
python3 magisk/tests/test_action.py
```

The fixture uses the bundled official x86_64 BusyBox and real utility script but
substitutes a non-flashing entrypoint. It checks RAM selection, protocol arguments,
paths with spaces, upstream nonzero status, rejected layouts and corrupt ZIPs.
`tests/verify_package_test.cpp` exercises the production SHA256 function, with
unused Android runtime sections discarded by the host link.

On 2026-10-10: eight adapter tests (including complete tool/asset extraction and architecture selection), the
Bionic ownership-contract regression, Magisk action syntax, and native checksum valid/corrupt/truncated/empty
cases pass; native installer and menu syntax checks pass against checkout headers
with Clang and `-Wall -Wextra -Werror`. Host checks do not prove SELinux/partition
compatibility or successful rooting. Recovery-image size/linking and phone testing
remain pending. No ROM build, flash, or access to `~/evo/out` was performed.

## Bundled tools

The APK contains ARM64 and ARM32 `busybox`, `bootctl`, `magisk`, `magiskboot`,
`magiskinit`, `magiskpolicy` and `init-ld`, plus boot-patching/utilities scripts,
manager stub and upstream ChromeOS resources. The adapter extracts all `assets/*`
and `lib/*`, excluding unused BusyBox copies; the native runner provides the
matching BusyBox. All are inside recovery's bundled APK and staged into RAM.

The user reported immediate return to the main recovery page after confirming
Magisk and unauthorized ADB after Enable ADB. Live logs were unavailable. The
ownership bug is source-confirmed and reproduced by a host contract test using
real libbase Bionic tagging; live acceptance of these fixes is pending a rebuilt
recovery image. Do not describe the host model as an actual Android crash capture.

## Platform detection after mounting Android

The next device test reached the upstream installer without restarting recovery,
selected Boot B, then showed a blank Device platform and failed at unpacking.
ADB now connects in recovery and reports `arm64-v8a`, SDK 37, user/Enforcing.
Root-only recovery logs remain unreadable to shell, and shell cannot stage a
non-flashing test under `/tmp`; no permissions or policy were weakened.

Capture SDK in the native parent and use the recovery executable's compiled ABI
before the child mounts Android over `/system`. Export `EVO_MAGISK_API` and
`EVO_MAGISK_ABI`, and override extracted `api_level_arch_detect()` with those
values and the matching ARCH/ABI32/IS64BIT mapping. This avoids reliance on
Android's mounted build.prop/product properties or a getprop/toolbox whose
libraries may be unavailable in recovery. Invalid/missing metadata is rejected
before patching. Host tests cover all four shipped architectures with property
helpers deliberately unavailable, and invalid metadata. Live installation
acceptance of this platform handoff still requires the rebuilt recovery image.
