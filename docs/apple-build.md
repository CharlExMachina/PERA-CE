# Build for iPhone

CoreCE's iPhone app ([port/ios](../port/ios/README.md)) runs the game as the
same ILP32 AArch64 guest as Android's, rebased for a signed image, and draws
with the OpenGL ES renderer through ANGLE's Metal backend. Its host is in
`port/ios/host` and `port/apple` (`port/apple/host`, `compiler`, `native`).
All commands run on an Apple silicon Mac from the repository root.

The Mac build is a different port: [port/macos](../port/macos/README.md),
drawn with Metal directly (`ninja macos`). It needs none of this page.

## 1. Public dependencies

Install full Xcode 26 or later, open it once to accept its licence, and
install the iOS platform support (and an iOS 26+ simulator runtime for
simulator builds, in Xcode's Settings → Components).

Using [Homebrew](https://brew.sh/):

```sh
brew install python cmake llvm@22 lld@22 sdl3 ninja
export HALO_IOS_LLVM_BIN="$(brew --prefix llvm@22)/bin"
export PATH="$(brew --prefix lld@22)/bin:$PATH"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer

python3 tools/ios_setup.py
```

The compiler pass (`port/apple/compiler/guest_rebase.cpp`) needs **LLVM 22**,
not Apple clang or a newer LLVM. Do not put LLVM's `bin` ahead of the system
compiler: the host builds with Apple's clang, and `HALO_IOS_LLVM_BIN` selects
the game's compiler. Python 3.10+ and CMake 3.28+ are needed. Setup accepts an
ELF-capable `ld.lld` on `PATH`; without one, `uv` enables a local Zig/LLD
fallback.

Setup downloads only public dependencies, into `build/apple`: ANGLE and the
Khronos headers, with the pinned URLs and SHA-256 checksums of
[dependencies.json](../port/apple/dependencies.json). The build also fetches
SDL3 and musl, as the Android build does. No Android NDK is needed.

## 2. Game data

The app does not include game data: on first launch it asks for your own Xbox
disc image (`.iso` or `.xiso`). Supported Xbox version-5 maps are PAL
`01.01.14.2342` and USA `01.10.12.2276`; use one complete set. Halo PC, Custom
Edition, Anniversary and MCC maps are not substitutes.

## 3. Build, sign and install

See [port/ios/README.md](../port/ios/README.md#build): a device build needs
your own paid Apple development team, a unique bundle identifier and a phone
in Developer Mode (a Personal Team could not provision the Extended Virtual
Addressing entitlement in testing). The [simulator](../port/ios/README.md#simulator)
is useful for menu and touch checks without signing.

## Checks

```sh
python3 tools/ios_build.py --plugin-only
python3 tools/test_ios_runtime.py
python3 tools/ios_mac_runtime_check.py   # the iOS runtime against the Mac's SDL
```

`test_ios_runtime.py` builds production device IR and runs the same
signed-code, TLS and indirect-call lowering in a read-only signed Mach-O image
on the Mac. It does not establish iPhone gameplay. `.github/workflows/apple.yml`
runs these on an ARM64 macOS runner.

## Source and build artifacts

`assets/`, `xbox/` and `build/` are ignored, along with disc images, map
files, Apple signing material and generated Xcode projects. Pass your team ID
and bundle identifier on the command line; do not commit your team,
certificate, device IDs or provisioning profile. The checked-in entitlement
file holds only a capability flag. Keep compiled apps, logs and provisioning
profiles local.
