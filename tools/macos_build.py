"""Ninja rules for the macOS build (``ninja macos``).

The macOS port (port/macos/README.md) runs the game as ILP32 AArch64 code -
32-bit pointers, as the game's data formats require - in an ordinary
64-bit Apple silicon app, as the Android port does. macOS keeps the low 4 GB
of every 64-bit process unmapped (__PAGEZERO), so the guest runs in a window
of 4 GB the host reserves elsewhere, its code rewritten to address memory
through the window's base (tools/macos_asm_rebase.py). This graph builds

- the guest image, build/macos/halo_guest.elf: the game sources, the
  platform layer shared with the Linux port (port/linux/src), the ILP32
  runtime shared with the Android port (port/android/guest) and musl,
  compiled by clang for arm64_32-apple-watchos with x27 and x28 reserved,
  converted to ELF assembly (tools/android_asm_convert.py), rebased onto the
  window (tools/macos_asm_rebase.py), assembled and linked at a fixed
  address, then checked (tools/macos_image_check.py);
- the host, an arm64 macOS executable: the loader and the services the
  guest calls, over SDL3 and Metal (port/macos/host);
- SDL3, a static library built with CMake;

and the app, build/macos/OpenCE.app.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

from .android_build import (EXPAT_DIR, EXPAT_SOURCES, KCP_DIR, MBEDTLS_DIR, MONOCYPHER_DIR, MUSL_VERSION, SDL_TAG,
                            SDL_URL, TOML_DIR, VARIADIC_PROTOTYPE_FILES, ZLIB_DEFINES, ZLIB_DIR, ZLIB_SOURCES,
                            GUEST_CODE_FLAGS, _musl_sources)
from .embed_assets import hud_assets_build, hud_configure_inputs
from .linux_build import (MINIUPNPC_DEFINES, MINIUPNPC_DIR, MUSL_MATH_DIR, STB_VORBIS_SOURCE, XDK_INCLUDE,
                          compile_launcher, configuration_defines, game_defines_and_includes, game_sources,
                          miniupnpc_sources, musl_math_sources, opus_cflags, opus_sources, updater_defines,
                          xdk_headers)
from .ninja_syntax import Writer

PORT_DIR = Path("port/macos")
ANDROID_DIR = Path("port/android")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/macos")
THIRD_PARTY = BUILD / "third_party"
MUSL_DIR = THIRD_PARTY / f"musl-{MUSL_VERSION}"
MUSL_URL = f"https://musl.libc.org/releases/musl-{MUSL_VERSION}.tar.gz"
SDL_DIR = THIRD_PARTY / "SDL3"
APP_NAME = "OpenCE"
# the oldest macOS the host runs on (SDL 3 and Metal 3 need less)
MACOS_MINIMUM = "13.0"

# The guest ABI: as the Android guest's (tools/android_build.py), but for
# the Mac's own cores, with the two registers the rebased code keeps for
# itself (the window's base in x28, scratch in x27) left alone by the
# compiler, and the Metal renderer in place of OpenGL ES.
GUEST_ABI_FLAGS = [
    "--target=arm64_32-apple-watchos",
    "-U__APPLE__",
    "-U__MACH__",
    "-fno-define-target-os-macros",
    "-D__linux__=1",
    "-D__unix__=1",
    # the ILP32 guest's code paths, the Metal renderer's and the Mac app's
    "-DHALO_ARM64_GUEST=1",
    "-DHALO_METAL=1",
    "-DHALO_MACOS=1",
    "-mcpu=apple-m1",
    "-ffixed-x27",
    "-ffixed-x28",
    "-nostdinc",
    "-fshort-wchar",
    "-fno-stack-protector",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    "-femulated-tls",
    "-mllvm",
    "-aarch64-neon-syntax=generic",
    # jump tables of 32-bit entries: the rebased code is longer than the
    # compiler measured it, out of the reach of its smaller ones
    "-mllvm",
    "-aarch64-enable-compress-jump-tables=0",
    # no fused multiply-add (tools/android_build.py)
    "-ffp-contract=off",
    "-O2",
]

FRAMEWORKS = [
    "Cocoa", "Metal", "QuartzCore", "IOKit", "CoreAudio", "AudioToolbox", "CoreVideo", "CoreFoundation",
    "Carbon", "ForceFeedback", "GameController", "CoreHaptics", "AVFoundation", "CoreMedia",
    "UniformTypeIdentifiers", "Security",
]


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def _llvm_tool(name: str) -> Optional[str]:
    """an LLVM tool that reads ELF (Apple's own tools read Mach-O only)"""
    found = shutil.which(name)
    if found:
        return found
    for prefix in ("/opt/homebrew/opt/llvm/bin", "/usr/local/opt/llvm/bin", "/opt/homebrew/opt/lld/bin",
                   "/usr/local/opt/lld/bin"):
        candidate = Path(prefix) / name
        if candidate.is_file():
            return str(candidate)
    return None


def fetch_third_party() -> None:
    """Download musl and SDL3 (configure time, once)."""
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    if not MUSL_DIR.is_dir():
        print(f"Downloading {MUSL_URL}")
        archive = THIRD_PARTY / f"musl-{MUSL_VERSION}.tar.gz"
        subprocess.run(["curl", "-sSfL", "-o", str(archive), MUSL_URL], check=True)
        subprocess.run(["tar", "xzf", archive.name], cwd=THIRD_PARTY, check=True)
        archive.unlink()
    if not SDL_DIR.is_dir():
        print(f"Cloning SDL3 {SDL_TAG}")
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--branch", SDL_TAG, SDL_URL, str(SDL_DIR)],
                       check=True)


def macos_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR / "host", PORT_DIR / "guest", ANDROID_DIR / "guest" / "runtime",
            LINUX_DIR / "src", *hud_configure_inputs()]


def generate_macos_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if sys.platform != "darwin" or not config_path.is_file() or not (PORT_DIR / "host").is_dir():
        return
    lld = _llvm_tool("ld.lld")
    llvm_ar = _llvm_tool("llvm-ar")
    objdump = _llvm_tool("llvm-objdump")
    if not lld or not llvm_ar:
        n.comment("macOS build: ld.lld and llvm-ar are needed (brew install lld llvm)")
        return
    if not shutil.which("cmake"):
        n.comment("macOS build: CMake is needed (brew install cmake)")
        return
    try:
        fetch_third_party()
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"macOS build disabled: cannot fetch musl/SDL3 ({error})", file=sys.stderr)
        return
    import json
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))

    guest_cc = getattr(sln, "macos_guest_cc", None) or "clang"
    host_cc = "clang"
    guest_dir = BUILD / "guest"
    obj_dir = guest_dir / "obj"
    gen_dir = guest_dir / "gen"
    libc_include = guest_dir / "libc_include"
    libc_internal = guest_dir / "libc_internal"
    arch = ANDROID_DIR / "guest" / "libc" / "arch" / "arm64_32"
    semantics_header = Path("build/linux/halo_msvc_semantics.h")
    platform_semantics_header = Path("build/linux/platform_msvc_semantics.h")
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    image = BUILD / "halo_guest.elf"
    sdl_build = BUILD / "sdl3-build"
    libsdl = sdl_build / "libSDL3.a"
    python = "$python"
    runtime_dir = ANDROID_DIR / "guest" / "runtime"
    macos_guest_dir = PORT_DIR / "guest"

    n.comment("macOS build (ninja macos); see port/macos/README.md")
    n.variable("macos_guest_cc", guest_cc)
    n.variable("macos_host_cc", host_cc)
    n.variable("macos_lld", lld)
    n.variable("macos_ar", llvm_ar)

    # ---------- generated headers and sources

    alltypes = libc_include / "bits" / "alltypes.h"
    syscall_h = libc_include / "bits" / "syscall.h"
    version_h = libc_internal / "version.h"
    n.rule(
        name="macos_alltypes",
        command=f"sed -f {MUSL_DIR}/tools/mkalltypes.sed $in > $out",
        description="MACOS MUSL $out",
    )
    n.build(outputs=alltypes, rule="macos_alltypes",
            inputs=[arch / "bits" / "alltypes.h.in", MUSL_DIR / "include" / "alltypes.h.in"])
    n.rule(
        name="macos_syscall_h",
        command="cp $in $out && sed -n -e s/__NR_/SYS_/p < $in >> $out",
        description="MACOS MUSL $out",
    )
    n.build(outputs=syscall_h, rule="macos_syscall_h", inputs=arch / "bits" / "syscall.h.in")
    n.rule(
        name="macos_version_h",
        command=f"echo '#define VERSION \"{MUSL_VERSION}\"' > $out",
        description="MACOS MUSL $out",
    )
    n.build(outputs=version_h, rule="macos_version_h")

    guest_posix_c = gen_dir / "guest_posix.c"
    posix_imports = gen_dir / "posix_imports.list"
    n.rule(
        name="macos_posix_stubs",
        command=f"{python} tools/android_posix_stubs.py {LINUX_DIR}/src/posix.h {guest_posix_c} {posix_imports}",
        description="MACOS POSIX STUBS",
    )
    n.build(outputs=[guest_posix_c, posix_imports], rule="macos_posix_stubs",
            implicit=[Path("tools/android_posix_stubs.py"), LINUX_DIR / "src" / "posix.h"])

    # the import stubs rebase the pointers they pass (tools/macos_imports.py),
    # from the prototypes in these headers
    imports_s = gen_dir / "imports.s"
    host_table_c = BUILD / "host" / "host_import_table.c"
    host_imports_list = PORT_DIR / "host_imports.list"
    prototype_headers = [runtime_dir / "guest_host.h", PORT_DIR / "include" / "macos_host.h",
                         PORT_DIR / "include" / "halo_metal.h", LINUX_DIR / "src" / "posix.h"]
    n.rule(
        name="macos_imports",
        command=(f"{python} tools/macos_imports.py --host-table {host_table_c} "
                 + " ".join(f"--prototypes {header}" for header in prototype_headers)
                 + f" {imports_s} $in"),
        description="MACOS IMPORTS",
    )
    n.build(outputs=[imports_s, host_table_c], rule="macos_imports",
            inputs=[host_imports_list, posix_imports],
            implicit=[Path("tools/macos_imports.py"), *prototype_headers])

    generated_headers = [*xdk_headers(), alltypes, syscall_h, version_h, semantics_header,
                         platform_semantics_header]

    # ---------- guest compilation: C -> Darwin assembly -> ELF assembly ->
    # rebased onto the window -> object

    n.rule(
        name="macos_guest_cc",
        command=(f"{compile_launcher(sln)}$macos_guest_cc -MMD -MF $out.d $cflags -S $in -o $out.darwin.s && "
                 f"{python} tools/android_asm_convert.py $out.darwin.s $out.elf.s && "
                 f"{python} tools/macos_asm_rebase.py $out.elf.s $out.s && "
                 "$macos_guest_cc --target=aarch64-linux-gnu -mcpu=apple-m1 -c $out.s -o $out"),
        description="MACOS CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    # assembly written for the window (the import stubs): not rebased again
    n.rule(
        name="macos_guest_as",
        command="$macos_guest_cc --target=aarch64-linux-gnu -mcpu=apple-m1 -c $in -o $out",
        description="MACOS AS $out",
    )

    libc_includes = [
        f"-isystem {libc_include}", f"-isystem {arch}", f"-isystem {MUSL_DIR}/arch/generic",
        f"-isystem {MUSL_DIR}/include",
    ]
    guest_abi = " ".join(GUEST_ABI_FLAGS + configuration_defines(sln))
    guest_code = " ".join(GUEST_CODE_FLAGS)
    tool_implicit = [Path("tools/android_asm_convert.py"), Path("tools/macos_asm_rebase.py"), *generated_headers]

    def guest_object(source: Path, cflags: str, prefix: str = "") -> Path:
        obj = obj_dir / prefix / Path(str(source).lstrip("/")).with_suffix(".o")
        if str(source).startswith(str(BUILD)):
            obj = obj_dir / prefix / source.relative_to(BUILD).with_suffix(".o")
        n.build(outputs=obj, rule="macos_guest_cc", inputs=source, implicit=tool_implicit,
                variables={"cflags": cflags})
        return obj

    # musl
    musl_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-w",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{ANDROID_DIR}/guest/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    musl_objects = [guest_object(source, musl_cflags, "musl") for source in _musl_sources(MUSL_DIR)]
    libguestc = guest_dir / "libguestc.a"
    n.rule(
        name="macos_ar",
        command="rm -f $out && $macos_ar rcs $out @$out.rsp",
        description="MACOS AR $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=libguestc, rule="macos_ar", inputs=musl_objects)

    # the game
    objects: List[Path] = []
    game_flags = [
        "-std=gnu89", "-D__STRICT_ANSI__", "-w",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types",
        "-Wno-error=int-conversion",
        "-Wno-error=implicit-function-declaration",
        "-Wno-error=implicit-int",
        "-Wno-error=return-type",
    ]
    game_cflags = " ".join([
        guest_abi, guest_code, " ".join(game_flags),
        f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{LINUX_DIR}/include",
        f"-iquote {Path(config['game_sources'])}",
        game_defines_and_includes(config), *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    for source in game_sources(config):
        cflags = game_cflags
        if source.as_posix() == "source/main/main.c":
            cflags += " " + updater_defines(getattr(sln, "port_release", False))
        if source.as_posix() in VARIADIC_PROTOTYPE_FILES:
            cflags += f" -include {ANDROID_DIR}/include/halo_android_variadic_prototypes.h"
        objects.append(guest_object(source, cflags))
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        objects.append(guest_object(source, game_cflags))
    objects.append(guest_object(STB_VORBIS_SOURCE, game_cflags))

    # the platform layer shared with Linux, and the guest runtime
    platform_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w",
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include", f"-I{runtime_dir}", f"-I{macos_guest_dir}",
        f"-I{PORT_DIR}/include", f"-I{ANDROID_DIR}/include", f"-I{TOML_DIR}", f"-I{EXPAT_DIR}", f"-I{KCP_DIR}",
        f"-I{MONOCYPHER_DIR}", f"-I{ZLIB_DIR}", "-Isource -Isource/cseries",
        f"-I{SDL_DIR}/include", *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    # the GL renderer and its helpers stay out: Metal draws instead
    # (d3d8_metal.c, the host's host_metal.m)
    platform_excluded = {"memory_watch.c", "d3d8_gl.c", "gl_functions.c", "xgpu_post.c"}
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_") or source.name in platform_excluded:
            continue
        objects.append(guest_object(source, platform_cflags))
    for source in hud_assets_build(n, "macos", gen_dir / "hud_hires_assets.c"):
        objects.append(guest_object(source, platform_cflags))
    objects.append(guest_object(TOML_DIR / "tomlc17.c", platform_cflags))
    for name in EXPAT_SOURCES:
        objects.append(guest_object(EXPAT_DIR / name, platform_cflags))
    objects.append(guest_object(KCP_DIR / "ikcp.c", platform_cflags))
    for source in opus_sources():
        objects.append(guest_object(source, " ".join([opus_cflags(guest_abi), *libc_includes])))
    for name in ("monocypher.c", "monocypher-ed25519.c"):
        objects.append(guest_object(MONOCYPHER_DIR / name, platform_cflags))
    for name in ZLIB_SOURCES:
        objects.append(guest_object(ZLIB_DIR / name, " ".join([platform_cflags, *ZLIB_DEFINES])))
    musl_math_cflags = " ".join([
        guest_abi, "-std=gnu11", "-w", *libc_includes, f"-I{MUSL_MATH_DIR}/include",
        f"-include {MUSL_MATH_DIR}/include/libm.h",
    ])
    for source in musl_math_sources():
        objects.append(guest_object(source, musl_math_cflags))
    runtime_internal_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-D_GNU_SOURCE",
        f"-I{runtime_dir}", f"-I{ANDROID_DIR}/include", f"-I{PORT_DIR}/include",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{ANDROID_DIR}/guest/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    runtime_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE",
        f"-I{runtime_dir}", f"-I{macos_guest_dir}", f"-I{ANDROID_DIR}/include", f"-I{PORT_DIR}/include",
        f"-I{LINUX_DIR}/src", f"-I{SDL_DIR}/include", *libc_includes,
    ])
    for source in sorted(runtime_dir.glob("*.c")):
        if source.name in ("guest_thread.c", "guest_start.c"):
            objects.append(guest_object(source, runtime_internal_cflags))
        elif source.name == "guest_memory_watch.c":
            objects.append(guest_object(source, platform_cflags))
        else:
            objects.append(guest_object(source, runtime_cflags))
    # the Mac's own guest code: the Metal renderer, the SDL functions only the
    # desktop calls, the compiler's runtime functions
    for source in sorted(macos_guest_dir.glob("*.c")):
        if source.name.startswith("guest_"):
            objects.append(guest_object(source, runtime_cflags))
        else:
            objects.append(guest_object(source, platform_cflags))
    objects.append(guest_object(guest_posix_c, runtime_cflags))
    imports_o = obj_dir / "gen" / "imports.o"
    n.build(outputs=imports_o, rule="macos_guest_as", inputs=imports_s)
    objects.append(imports_o)

    # ---------- the guest image

    linker_script = ANDROID_DIR / "guest" / "guest.ld"
    unchecked_image = BUILD / "halo_guest.unchecked.elf"
    n.rule(
        name="macos_guest_link",
        command=(f"$macos_lld -m aarch64linux -static -nostdlib -T {linker_script} "
                 f"-Map $out.map -o $out @$out.rsp {libguestc}"),
        description="MACOS LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=unchecked_image, rule="macos_guest_link", inputs=objects, implicit=[libguestc, linker_script])
    # every memory access and indirect branch of the image goes through the
    # window (tools/macos_image_check.py), else the image is not made
    if objdump:
        n.variable("macos_objdump", objdump)
        n.rule(
            name="macos_image_check",
            command=f"{python} tools/macos_image_check.py $macos_objdump $in && cp $in $out",
            description="MACOS CHECK $out",
        )
        n.build(outputs=image, rule="macos_image_check", inputs=unchecked_image,
                implicit=[Path("tools/macos_image_check.py")])
    else:
        n.comment("(no llvm-objdump: the image is not checked)")
        n.rule(name="macos_copy_image", command="cp $in $out", description="MACOS IMAGE $out")
        n.build(outputs=image, rule="macos_copy_image", inputs=unchecked_image)

    # ---------- SDL3

    n.rule(
        name="macos_sdl3",
        command=(f"cmake -S {SDL_DIR} -B {sdl_build} -G Ninja -DCMAKE_BUILD_TYPE=Release "
                 f"-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_MINIMUM} "
                 "-DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "
                 # the game draws with Metal itself: SDL's renderers and its
                 # OpenGL are not needed
                 "-DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF "
                 f"> {BUILD}/sdl3-configure.log && ninja -C {sdl_build} > {BUILD}/sdl3-build.log"),
        description="MACOS SDL3",
        pool="console",
    )
    n.build(outputs=libsdl, rule="macos_sdl3", implicit=[SDL_DIR / "CMakeLists.txt"])

    # ---------- the host

    host_objects: List[Path] = []
    host_obj_dir = BUILD / "host" / "obj"
    n.rule(
        name="macos_host_cc",
        command="$macos_host_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="MACOS HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    host_base_flags = [
        "-arch arm64", f"-mmacosx-version-min={MACOS_MINIMUM}", "-O2", "-g", "-Wall", "-Wno-unused-function",
        f"-I{PORT_DIR}/include", f"-I{PORT_DIR}/host", f"-I{ANDROID_DIR}/include", f"-I{SDL_DIR}/include",
        f"-I{LINUX_DIR}/src", f"-I{TOML_DIR}",
    ]
    host_cflags = " ".join(host_base_flags)
    host_objc_flags = " ".join(host_base_flags + ["-fobjc-arc", "-fmodules"])
    host_sources = sorted((PORT_DIR / "host").glob("*.c")) + sorted((PORT_DIR / "host").glob("*.m")) + \
        sorted((PORT_DIR / "host").glob("*.S")) + [
        LINUX_DIR / "src" / "posix_files.c", LINUX_DIR / "src" / "posix_net.c",
        TOML_DIR / "tomlc17.c",
    ]
    for source in host_sources:
        obj = host_obj_dir / (source.name + ".o")
        flags = host_objc_flags if source.suffix == ".m" else host_cflags
        n.build(outputs=obj, rule="macos_host_cc", inputs=source, variables={"cflags": flags},
                order_only=[libsdl])
        host_objects.append(obj)
    miniupnpc_cflags = " ".join([host_cflags, f"-I{MINIUPNPC_DIR / 'include'}", f"-I{MINIUPNPC_DIR / 'src'}",
                                 *MINIUPNPC_DEFINES])
    for source in [LINUX_DIR / "src" / "posix_upnp.c", *miniupnpc_sources()]:
        obj = host_obj_dir / ("miniupnpc_" + source.name + ".o" if source.parent.parent == MINIUPNPC_DIR
                              else source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source,
                variables={"cflags": miniupnpc_cflags + (" -w" if source.name != "posix_upnp.c" else "")})
        host_objects.append(obj)
    mbedtls_cflags = " ".join([host_cflags, f"-I{MBEDTLS_DIR / 'include'}", f"-I{MBEDTLS_DIR / 'library'}"])
    for source in [LINUX_DIR / "src" / "posix_dtls.c", *sorted((MBEDTLS_DIR / "library").glob("*.c"))]:
        obj = host_obj_dir / ("mbedtls_" + source.name + ".o" if source.parent.parent == MBEDTLS_DIR
                              else source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source,
                variables={"cflags": mbedtls_cflags + (" -w" if source.name != "posix_dtls.c" else "")})
        host_objects.append(obj)
    table_obj = host_obj_dir / "host_import_table.c.o"
    n.build(outputs=table_obj, rule="macos_host_cc", inputs=host_table_c, variables={"cflags": host_cflags})
    host_objects.append(table_obj)
    host_executable = BUILD / "host" / "halo"
    n.rule(
        name="macos_host_link",
        command=(f"$macos_host_cc -arch arm64 -mmacosx-version-min={MACOS_MINIMUM} -o $out $in {libsdl} "
                 + " ".join(f"-framework {framework}" for framework in FRAMEWORKS)
                 + " -liconv"),
        description="MACOS HOST LINK $out",
    )
    n.build(outputs=host_executable, rule="macos_host_link", inputs=host_objects, implicit=[libsdl])

    # ---------- the app

    app = BUILD / f"{APP_NAME}.app"
    contents = app / "Contents"
    n.rule(name="macos_copy", command="cp $in $out", description="MACOS STAGE $out")
    n.rule(
        name="macos_plist",
        command=f"sed -e 's/@MACOS_MINIMUM@/{MACOS_MINIMUM}/g' $in > $out",
        description="MACOS PLIST $out",
    )
    # the app's icon, from the project's (port/assets/icon), at the sizes
    # macOS asks an .icns for
    icon = contents / "Resources" / "OpenCE.icns"
    iconset = BUILD / "OpenCE.iconset"
    icon_sizes = [(16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2), (256, 1), (256, 2), (512, 1), (512, 2)]
    n.rule(
        name="macos_icon",
        command=(f"rm -rf {iconset} && mkdir -p {iconset} && "
                 + " && ".join(f"sips -z {size * scale} {size * scale} $in --out "
                               f"{iconset}/icon_{size}x{size}{'@2x' if scale == 2 else ''}.png > /dev/null"
                               for size, scale in icon_sizes)
                 + f" && iconutil -c icns {iconset} -o $out"),
        description="MACOS ICON $out",
    )
    n.build(outputs=icon, rule="macos_icon", inputs=Path("port/assets/icon/opence-icon.png"))
    staged = [contents / "MacOS" / "halo", contents / "Resources" / "halo_guest.elf",
              contents / "Resources" / "brokers.txt", contents / "Info.plist", icon]
    n.build(outputs=staged[0], rule="macos_copy", inputs=host_executable)
    n.build(outputs=staged[1], rule="macos_copy", inputs=image)
    n.build(outputs=staged[2], rule="macos_copy", inputs=Path("port/assets/network/brokers.txt"))
    n.build(outputs=staged[3], rule="macos_plist", inputs=PORT_DIR / "app" / "Info.plist")
    # (signed as a whole, ad hoc: Apple silicon runs only signed code; the
    # signature does not use the hardened runtime, which would refuse the
    # guest's code its executable pages without an entitlement)
    signature = contents / "_CodeSignature" / "CodeResources"
    n.rule(
        name="macos_sign",
        command=f"codesign --force --sign - {_quote(app)} > /dev/null",
        description="MACOS SIGN $out",
    )
    n.build(outputs=signature, rule="macos_sign", inputs=staged)
    n.build(outputs="macos", rule="phony", inputs=[signature])
    n.newline()
