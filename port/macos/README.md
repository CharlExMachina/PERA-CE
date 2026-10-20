# macOS

`ninja macos` builds the game for Macs with Apple silicon:
`build/macos/OpenCE.app`.

The game is native ARM code. It draws with Metal and plays sound and reads
controllers through SDL3 (Core Audio, Game Controller). It needs macOS 13 or
later. Intel Macs do not run it.

The macOS build uses the platform layer of the Linux build
(`port/linux/src`) and the ILP32 runtime of the Android build
(`port/android/guest`). Refer to [port/linux/README.md](../linux/README.md)
for the controls, the settings and the multiplayer functions, which are the
same on the Mac.

## Requirements

You do not need the Xbox SDK or Xcode. You need:

- The Command Line Tools for Xcode (`xcode-select --install`): the clang of
  the system has the `arm64_32` target the game is compiled for, and the
  Metal framework. The game compiles its shaders while it runs, so the
  Metal compiler of Xcode is not needed.
- Python 3, [ninja](https://ninja-build.org/) and CMake.
- LLVM's linker and tools, which read ELF: `ld.lld`, `llvm-ar` and
  `llvm-objdump`. Homebrew has them: `brew install ninja cmake lld llvm`.
- A network connection for the first build. `configure.py` downloads musl
  1.2.5 and SDL 3.4.16 to `build/macos/third_party`.

The option `--macos-guest-cc` of `configure.py` selects another clang for
the game's code.

## Build and run

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja macos` (on a Mac, `ninja` alone builds it too).
4. Open `build/macos/OpenCE.app`, or enter
   `build/macos/OpenCE.app/Contents/MacOS/halo`.

The app is signed ad hoc (without a developer identity). macOS runs it on
the Mac that built it. A copy downloaded from elsewhere needs the right
click, Open, at its first start (or `xattr -dr com.apple.quarantine
OpenCE.app`).

## Game data and files

At the first start, the game asks for an Xbox disc image (`.xiso` or
`.iso`) and extracts its `maps/` folder (refer to the README of the
repository). The game keeps its files in `~/Library/Application
Support/OpenCE`:

| Item | Location in `~/Library/Application Support/OpenCE` |
| --- | --- |
| Game data | `maps` |
| Saved games (`z:\` and `u:\`) | `saves` |
| Settings | `config.toml` |
| Log | `debug.txt` |
| Mods | `mods` |
| Screenshots | `screenshots` |

Photo mode's photos (refer to "Photo mode" in
[port/linux/README.md](../linux/README.md)) go in `~/Pictures/OpenCE`.

Started from a folder that holds `maps/` (or `assets/maps/`, as the root of
the repository), the game plays those maps instead, as the Linux build does.
`paths.data` and `paths.saves` in `config.toml` choose other folders.

## How the port operates

### ILP32 code in a window

The game's data contains the Xbox's 32-bit pointers (refer to "ILP32 code" in
[port/android/README.md](../android/README.md)). As on Android, the game is
compiled as ILP32 AArch64 code: 64-bit ARM instructions with 32-bit `int`,
`long` and pointers, run directly by the CPU.

Android maps the game at its own addresses, below 4 GB. macOS does not let a
64-bit process map anything below 4 GB (`__PAGEZERO` takes them all). The
game instead runs in a window of 4 GB of the process's address space, at a
4 GB boundary: guest address A is host address window + A. Its code keeps
the window's base in `x28`. `tools/macos_asm_rebase.py` rewrites the
compiler's assembly so that every load, store and indirect branch goes
through the base:

```
ldr w0, [x1, #8]        ->  add x27, x28, w1, uxtw
                            ldr w0, [x27, #8]
ldr w0, [x1, x2, lsl #2] -> add w27, w1, w2, lsl #2
                            ldr w0, [x28, w27, uxtw]
```

The compiler leaves `x27` and `x28` alone (`-ffixed-x27 -ffixed-x28`).
Registers hold guest addresses, as the code expects, except the stack
pointer and the frame pointer, which hold host addresses: the stacks are in
the window. An instruction that makes a pointer from them, or from the
program counter (`adrp`, `adr`), is followed by `mov wN, wN`, which clears
the base. The rewrite stops the build at anything it does not understand,
and `tools/macos_image_check.py` checks the linked image: no load, store or
branch of it can reach memory outside the window.

The 4 GB below the window and above it are never mapped, nor are the
window's first 1 MB: a null pointer faults, as it does elsewhere.

### The guest image

The guest is built as Android's is (refer to "The guest image" in
[port/android/README.md](../android/README.md)): clang compiles it for
`arm64_32-apple-watchos`; `tools/android_asm_convert.py` makes ELF assembly
of it; `tools/macos_asm_rebase.py` rebases that; `ld.lld` links it at
`0x88000000` to `build/macos/halo_guest.elf`, which the app carries in
`Contents/Resources`.

The guest's musl has 16 KB pages, Apple silicon's (`PAGESIZE` in
`port/android/guest/libc/arch/arm64_32/bits/limits.h`).

### The host

The executable is an ordinary arm64 macOS program (`port/macos/host`). It:

- Reserves the window, maps the Xbox memory (`0x80000000`, 128 MB) and the
  image in it, and hands out pages for the guest's other mappings
  (`host_memory.c`). The game maps and protects the Xbox memory 4 KB at a
  time, and a 16 KB page holds four of its blocks: there, mappings only
  clear what they map, and protection is the memory watch's.
- Loads the image and fills its import table (`host_loader.c`).
- Runs the game's `main` on the process's main thread, which Cocoa needs
  for the window and its events, on a stack in the window
  (`host_thread.c`, `host_entry.S`). Every other thread that runs guest code
  has its stack in the window too. `host_enter_guest` sets `x28` and keeps
  the host's own `x27` and `x28`.
- Performs musl's Linux system calls with Darwin's (`host_syscall.c`): the
  numbers, flags, structures and `errno` values differ. Futexes are
  Darwin's `__ulock` calls; directories are read with `readdir`.
- Does the guest's SDL calls (`host_sdl.c`), and the file, socket, UPnP
  and DTLS functions of `port/linux/src/posix_*.c`, which are compiled for
  Darwin.
- Tracks the game's writes to the Xbox memory by page protection
  (`host_watch.c`), at 16 KB pages: a write next to a cached texture makes it
  stale too, which costs an upload but is never wrong.

The guest calls the host through import stubs (`tools/macos_imports.py`).
A stub turns the pointers the guest passes into host addresses, from the
function's prototype: the host's functions take ordinary pointers, and
return guest addresses or handles.

To the guest, the folder its executable is in (`/proc/self/exe`,
`SDL_GetBasePath`) is `~/Library/Application Support/OpenCE`, where the
desktop ports keep their files beside the executable.

### Metal

The Direct3D device of the macOS build is `port/macos/guest/d3d8_metal.c`.
It is the device of `port/linux/src/d3d8_gl.c` with Metal in place of
OpenGL: the vertex programs and the pixel shaders are translated into Metal
shading language (`nv2a_vsh.c` and `nv2a_psh.c` write it under
`HALO_METAL`, from the same translation as their GLSL), and the state of
each draw goes to the host's Metal (`port/macos/host/host_metal.m`) as one
structure (`port/macos/include/halo_metal.h`). The host owns the window's
`CAMetalLayer`, the command buffers and the objects.

What differs from OpenGL:

- Metal runs a frame's work after it is asked for. A draw's vertices and
  indices are copied as it is drawn: those that do not change are kept in
  the mirror, buffers of guest memory the GPU reads in place, copied again
  only after the game writes them and the GPU has finished the frames that
  read them; the others go into the frame's stream chunks.
- Pipeline states hold what OpenGL sets as it goes (the vertex layout, the
  blend, the formats of the targets). Each is made once for its
  combination and kept, as are samplers and depth and stencil states. A
  shader is compiled at its first draw, as with OpenGL.
- Metal has no constant vertex attributes. The vertex shader reads the
  attributes that no stream feeds from its uniforms.
- A draw that samples its own render target samples a copy of it.
- The depth buffers are 32-bit float with 8-bit stencil: Apple's GPUs have
  no 24-bit depth.
- Clears draw a rectangle, of the channels the game names.
- `display.anti_aliasing`: `msaa2x` and `msaa4x` (the GPU's most) draw the
  screen's targets multisampled; `ssaa2x` draws them at twice the size;
  `fxaa` is a pass over each window's 3D view, and `smaa` draws FXAA too,
  as on Android.

### Game source changes

The macOS build needs no change in `source/`. It compiles the game with
`HALO_ARM64_GUEST`, as Android does (refer to "Game source changes" in
[port/android/README.md](../android/README.md)).

The port's code tells apart, each its own macro (`tools/macos_build.py`):

- `HALO_ARM64_GUEST`: the guest's ABI;
- `HALO_METAL`: the Metal renderer;
- `HALO_MACOS`: the Mac app (its files, its window's icon, no self-updater
  yet).

## Find problems

- The host's log and the game's go to standard error: start the app from a
  terminal (`build/macos/OpenCE.app/Contents/MacOS/halo`). `debug.txt` is
  the game's log.
- If the guest code stops, the log shows the registers and the guest's
  frames, as guest addresses. To find the functions, enter
  `llvm-symbolizer --obj=build/macos/halo_guest.elf <address>`.
- `debug.gpu_stats` logs the draws, passes and copies each second;
  `debug.gpu_dump_shaders` writes the Metal shaders it compiles into a
  folder; `MTL_DEBUG_LAYER=1` turns on Metal's own checks.
- In lldb, the memory watch's faults stop the game: enter
  `process handle SIGBUS SIGSEGV -n false -p true -s false` first.

## Tests

- `python -m pytest -q tools/test_macos_asm_rebase.py`: the rewrite of each
  addressing form, what it refuses, and, on an Apple silicon Mac, a guest
  built as the port builds it, run in a window.

## Limits

- Bink video is not available. The game skips the movies.
- The app does not update itself (`port/linux/src/updater.c`), and invite
  links do not open it: start the game and paste the link (the game takes it
  from the clipboard).
- SMAA draws FXAA (above).
- The app does not use the hardened runtime, under which its own memory
  could not become executable code. A build to notarize needs the
  `com.apple.security.cs.allow-unsigned-executable-memory` entitlement.
