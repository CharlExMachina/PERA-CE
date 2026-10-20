"""The macOS guest's rebasing onto its window (tools/macos_asm_rebase.py),
its import stubs (tools/macos_imports.py) and the image check
(tools/macos_image_check.py): each addressing form rewritten through x28,
what the rewrite refuses, and, on an Apple silicon Mac, a guest built as the
port builds it running in a window (tools/tests/macos_window)."""
import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from tools.macos_asm_rebase import Rebaser, RebaseError
from tools.macos_imports import classify

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tools" / "tests" / "macos_window"


def rebase(*lines):
    return [line.strip() for line in Rebaser(list(lines)).run().splitlines() if line.strip()]


@pytest.mark.parametrize("line, expected", [
    ("\tldr w0, [x1]", ["add x27, x28, w1, uxtw", "ldr w0, [x27]"]),
    ("\tldr w0, [x1, #8]", ["add x27, x28, w1, uxtw", "ldr w0, [x27, #8]"]),
    ("\tldur w0, [x1, #-4]", ["add x27, x28, w1, uxtw", "ldur w0, [x27, #-4]"]),
    ("\tldp x0, x1, [x2, #16]", ["add x27, x28, w2, uxtw", "ldp x0, x1, [x27, #16]"]),
    ("\tldr w8, [x8, :lo12:table]", ["add x27, x28, w8, uxtw", "ldr w8, [x27, :lo12:table]"]),
    # a register offset is added in 32 bits, whichever register is the pointer
    ("\tldr w0, [x1, x2, lsl #2]", ["add w27, w1, w2, lsl #2", "ldr w0, [x28, w27, uxtw]"]),
    ("\tstrb w10, [x8, w9, sxtw]", ["add w27, w8, w9", "strb w10, [x28, w27, uxtw]"]),
    ("\tstrh wzr, [x19, wzr, sxtw #1]", ["add x27, x28, w19, uxtw", "strh wzr, [x27]"]),
    # writeback on the register itself
    ("\tldr w0, [x1, #4]!", ["add x1, x1, #4", "add x27, x28, w1, uxtw", "ldr w0, [x27]"]),
    ("\tstr x0, [x1, #-16]!", ["sub x1, x1, #16", "add x27, x28, w1, uxtw", "str x0, [x27]"]),
    ("\tldr w0, [x1], #4", ["add x27, x28, w1, uxtw", "ldr w0, [x27]", "add x1, x1, #4"]),
    ("\tld1 { v0.4s }, [x0], x3", ["add x27, x28, w0, uxtw", "ld1 { v0.4s }, [x27]", "add x0, x0, x3"]),
    ("\tld1 { v0.s }[1], [x8]", ["add x27, x28, w8, uxtw", "ld1 { v0.s }[1], [x27]"]),
    ("\tldaxr w8, [x0]", ["add x27, x28, w0, uxtw", "ldaxr w8, [x27]"]),
    ("\tcasal w8, w9, [x0]", ["add x27, x28, w0, uxtw", "casal w8, w9, [x27]"]),
    ("\tprfm pldl1keep, [x8, #64]", ["add x27, x28, w8, uxtw", "prfm pldl1keep, [x27, #64]"]),
    # the stack is the window's already
    ("\tstp x29, x30, [sp, #-16]!", ["stp x29, x30, [sp, #-16]!"]),
    ("\tldur w8, [x29, #-4]", ["ldur w8, [x29, #-4]"]),
    ("\tblr x8", ["add x27, x28, w8, uxtw", "blr x27"]),
    ("\tbr x16", ["add x27, x28, w16, uxtw", "br x27"]),
    ("\tret", ["ret"]),
    # pointers made from the program counter or the stack lose the base
    ("\tadrp x8, table", ["adrp x8, table", "mov w8, w8"]),
    ("\tadd x8, sp, #12", ["add x8, sp, #12", "mov w8, w8"]),
    ("\tsub x0, x29, #4", ["sub x0, x29, #4", "mov w0, w0"]),
    # and gain it back on the way into sp
    ("\tmov sp, x8", ["add x27, x28, w8, uxtw", "mov sp, x27"]),
    ("\tsub sp, sp, x9", ["sub sp, sp, x9"]),
    # x30 is any register once the return address is saved
    ("\tmov x30, x24", ["mov x30, x24"]),
    ("\tldr x30, [x19, #144]", ["add x27, x28, w19, uxtw", "ldr x30, [x27, #144]"]),
    ("\tadd w0, w1, w2", ["add w0, w1, w2"]),
])
def test_rewrites(line, expected):
    assert rebase(line) == expected


@pytest.mark.parametrize("line", [
    "\tmov x27, x0",
    "\tadd x0, x28, #8",
    "\tstr x29, [x8]",
    "\tcmp sp, x9",
])
def test_refusals(line):
    with pytest.raises(RebaseError):
        rebase(line)


def test_labels_and_directives_pass():
    assert rebase(".text", "f:", "\t.p2align 2", "\tret") == [".text", "f:", ".p2align 2", "ret"]


def test_import_pointers():
    assert classify("f", "const char *name, int value, void *data, float x, unsigned int handle") == [0, 2]
    assert classify("posix_directory_next", "void *directory, char *name, posix_ulong name_size") == [1]
    assert classify("g", "void") == []


def tool(name):
    found = shutil.which(name)
    for prefix in ("/opt/homebrew/opt/llvm/bin", "/opt/homebrew/opt/lld/bin", "/usr/local/opt/llvm/bin"):
        if not found and (Path(prefix) / name).is_file():
            found = str(Path(prefix) / name)
    return found


def test_guest_runs_in_its_window(tmp_path):
    """the prototype of the port: the guest's code, built as the port builds
    it, runs from its window and computes what it computes natively"""
    if sys.platform != "darwin" or platform.machine() != "arm64":
        pytest.skip("an Apple silicon Mac runs the guest")
    clang, lld, objdump = shutil.which("clang"), tool("ld.lld"), tool("llvm-objdump")
    if not clang or not lld:
        pytest.skip("clang and ld.lld are needed")
    flags = ["--target=arm64_32-apple-watchos", "-ffixed-x27", "-ffixed-x28", "-O2", "-fno-omit-frame-pointer",
             "-fno-stack-protector", "-ffreestanding", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
             "-ffp-contract=off", "-mcpu=apple-m1", "-mllvm", "-aarch64-enable-compress-jump-tables=0"]
    darwin, converted, rebased = tmp_path / "guest.darwin.s", tmp_path / "guest.elf.s", tmp_path / "guest.s"
    subprocess.run([clang, *flags, f"-I{FIXTURE}", "-S", str(FIXTURE / "guest.c"), "-o", str(darwin)], check=True,
                   capture_output=True)
    subprocess.run([sys.executable, str(ROOT / "tools" / "android_asm_convert.py"), str(darwin), str(converted)],
                   check=True)
    subprocess.run([sys.executable, str(ROOT / "tools" / "macos_asm_rebase.py"), str(converted), str(rebased)],
                   check=True)
    imports = tmp_path / "imports.s"
    subprocess.run([sys.executable, str(ROOT / "tools" / "macos_imports.py"), "--prototypes",
                    str(FIXTURE / "imports.h"), str(imports), str(FIXTURE / "imports.list")], check=True)
    objects = []
    for source in (rebased, imports):
        obj = source.with_suffix(".o")
        subprocess.run([clang, "--target=aarch64-linux-gnu", "-mcpu=apple-m1", "-c", str(source), "-o", str(obj)],
                       check=True)
        objects.append(str(obj))
    image = tmp_path / "guest.elf"
    subprocess.run([lld, "-m", "aarch64linux", "-static", "-nostdlib", "-T", str(FIXTURE / "image.ld"), "-o",
                    str(image), *objects], check=True)
    if objdump:
        subprocess.run([sys.executable, str(ROOT / "tools" / "macos_image_check.py"), objdump, str(image)],
                       check=True)
    table = subprocess.run([tool("llvm-nm") or "nm", str(image)], check=True, capture_output=True, text=True).stdout
    address = next(line.split()[0] for line in table.splitlines() if line.endswith(" __host_import_table"))
    host = tmp_path / "host"
    subprocess.run([clang, "-O2", str(FIXTURE / "host.c"), "-o", str(host)], check=True)
    output = subprocess.run([str(host), str(image), address], check=True, capture_output=True, text=True).stdout
    results = dict(line.rsplit("=", 1) for line in output.splitlines())
    assert results == {
        "list sum (280)": "280",
        "two": "0",
        "ops (7,3,30,3)": "307330",
        "pick (66)": "66",
        "vla (4950*3=14850)": "14850",
        "dot (4032)": "4032",
        "copy (299%256=43)": "43",
        "is_head (1)": "1",
        "is_local (1)": "1",
        "stack string": "0",
        "va (15)": "15",
        "atomic (5)": "5",
        "ptr bits (stack < 4GB: 1)": "1",
        "result": "42",
    }
