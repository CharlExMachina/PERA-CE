#!/usr/bin/env python3
"""Check that the macOS guest image addresses memory only through its window.

tools/macos_asm_rebase.py rewrites every load, store and indirect branch of
the guest's code to go through the window's base (x28). An instruction it
did not see - from a file that skipped the rewrite, or a form it does not
know - would read or write the host's own memory at a guest address, or
branch there. This disassembles the linked image and allows only:

- loads and stores through sp, x29 or x27 (the rebased address), or at
  [x28, w27, uxtw];
- br and blr through x27; ret;
- in the import stubs and the linker's branch thunks, which are made for the
  window as they are, a load into x16 through x16 after an adrp of it, and
  a branch through x16;

and no write to x27 or x28 but the rebasing's own.

Usage: macos_image_check.py llvm-objdump image.elf
"""

import re
import subprocess
import sys

INSTRUCTION = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{8}\s+)?(\S+)\s*(.*)$")
SYMBOL = re.compile(r"^[0-9a-f]+ <([^>]+)>:")
MEMORY = re.compile(r"\[(x\d+|sp)\b")
NO_DESTINATION = {"b", "bl", "br", "blr", "ret", "cbz", "cbnz", "tbz", "tbnz", "cmp", "cmn", "tst", "ccmp", "ccmn",
                  "fcmp", "fcmpe", "prfm", "prfum"}


def writes_no_register(op: str) -> bool:
    """branches, compares and stores (whose first operand is read)"""
    return op in NO_DESTINATION or op.startswith("b.") or (op.startswith("st") and not op.startswith(("stxr",
        "stlxr", "stxp", "stlxp")))


def main():
    objdump, image = sys.argv[1:3]
    process = subprocess.Popen([objdump, "-d", "--no-show-raw-insn", "-j", ".text", image],
                               stdout=subprocess.PIPE, text=True, errors="replace")
    errors = []
    previous = ("", "")
    symbol = ""
    count = 0
    for line in process.stdout:
        m = SYMBOL.match(line)
        if m:
            symbol = m.group(1)
            previous = ("", "")
            continue
        m = INSTRUCTION.match(line)
        if not m:
            continue
        address, op, operands = m.group(1), m.group(2), m.group(3).split("//")[0].strip()
        count += 1
        problem = None
        # (a memory operand opens with its base register; a lane, v0.s[1],
        # with a number)
        memory_match = MEMORY.search(operands)
        if memory_match:
            memory = operands[memory_match.start():]
            base = memory_match.group(1)
            if base in ("sp", "x29", "x27"):
                pass
            elif base == "x28":
                if not re.match(r"\[x28, w27, uxtw\]", memory):
                    problem = "an access through x28 that is not the rebasing's"
            elif base == "x16" and op == "ldr" and previous[0] == "adrp" and previous[1].startswith("x16"):
                pass
            else:
                problem = f"an access through {base}"
        elif op in ("br", "blr", "braaz", "blraaz", "ret") and operands:
            target = operands.split(",")[0].strip()
            if target == "x27" or (op == "ret" and target == "x30"):
                pass
            elif target == "x16" and previous[0] in ("ldr", "add") and previous[1].startswith("x16"):
                pass
            else:
                problem = f"a branch through {target}"
        # (instructions that write no register: branches, compares, stores)
        if not problem and operands and not writes_no_register(op):
            destination = operands.split(",")[0].strip()
            if destination in ("x28", "w28"):
                problem = "a write to x28"
            elif destination in ("x27", "w27") and op != "add":
                problem = "a write to x27 that is not the rebasing's"
        if problem:
            errors.append(f"{address} <{symbol}>: {op} {operands}: {problem}")
        previous = (op, operands)
    process.wait()
    if process.returncode:
        sys.exit(f"{objdump} failed")
    if not count:
        sys.exit(f"{image}: no instructions found")
    if errors:
        shown = "\n".join(errors[:50])
        more = f"\n... and {len(errors) - 50} more" if len(errors) > 50 else ""
        sys.exit(f"{image}: {len(errors)} instructions escape the guest's window:\n{shown}{more}")


if __name__ == "__main__":
    main()
