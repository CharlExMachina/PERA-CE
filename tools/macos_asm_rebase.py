"""Rebase the macOS guest's AArch64 ELF assembly onto its address window.

The macOS port (port/macos/README.md) runs the game as ILP32 AArch64 code,
as the Android port does: 64-bit ARM instructions with 32-bit pointers, so
that the game's data (which embeds the Xbox's 32-bit pointers) keeps its
layout. Android maps the guest at its own addresses, below 4 GB. macOS on
Apple silicon gives every 64-bit process a 4 GB __PAGEZERO, so no memory can
be mapped there. The guest instead runs in a window of 4 GB that the host
reserves at a 4 GB-aligned address: guest address A is host address
window + A.

This script rewrites the assembly that tools/android_asm_convert.py makes
from clang's arm64_32 output so that the code works in the window. The
guest is compiled with x27 and x28 reserved (-ffixed-x27 -ffixed-x28); x28
holds the window's base, x27 is scratch.

- Every register that holds a pointer holds the guest address (its upper 32
  bits zero), as arm64_32 code expects. The exceptions are sp and x29 (the
  frame pointer), which hold host addresses: the guest's stacks are inside
  the window. x30 holds one too after a call (the return address: the code
  is inside the window as well), which only ret reads as it is; the
  compiler also gives x30 other values, as any register.
- A load or store through sp or x29 is left alone. Any other base register
  is rebased: `ldr w0, [x1, #8]` becomes `add x27, x28, w1, uxtw` and
  `ldr w0, [x27, #8]`. A register offset is added in 32 bits first, so that
  either register may hold the pointer: `ldr w0, [x1, x2, lsl #2]` becomes
  `add w27, w1, w2, lsl #2` and `ldr w0, [x28, w27, uxtw]`. Writeback is
  done on the original register.
- `br` and `blr` through a register are rebased the same way (a return
  address rebased is itself).
- An instruction that makes a pointer from sp, x29 or the program counter
  (adrp, adr, `add x0, sp, #16`) is followed by `mov w0, w0`, which clears
  the window's base from it.
- An instruction that sets sp or x29 from another register rebases that
  register first.

Anything else that involves sp, x29, x27 or x28 stops the build: the
rewrite must understand every instruction that could mix a host address
with a guest one. tools/macos_image_check.py checks the linked image.

Usage: macos_asm_rebase.py input.s output.s
"""

import re
import sys

BASE = "x28"
SCRATCH = "x27"
SCRATCH_W = "w27"

GPR64 = re.compile(r"^x([0-9]|[12][0-9]|30)$")
GPR32 = re.compile(r"^w([0-9]|[12][0-9]|30)$")
HOST_FORM = {"sp", "x29"}
RESERVED = {"x27", "w27", "x28", "w28"}

# instructions without a destination register
NO_DESTINATION = {
    "cmp", "cmn", "tst", "ccmp", "ccmn", "fcmp", "fcmpe", "fccmp", "fccmpe",
    "cbz", "cbnz", "tbz", "tbnz", "b", "bl", "br", "blr", "ret", "msr",
    "hint", "nop", "dmb", "dsb", "isb", "brk", "hlt", "udf", "yield", "clrex",
}


class RebaseError(Exception):
    pass


def split_operands(text: str):
    """Top-level comma split that keeps [...] and {...} whole."""
    parts, depth, current = [], 0, []
    for c in text:
        if c in "[{":
            depth += 1
        elif c in "]}":
            depth -= 1
        if c == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
        else:
            current.append(c)
    if current or parts:
        parts.append("".join(current).strip())
    return parts


def register_tokens(operand: str):
    return re.findall(r"\b(?:[xw](?:[0-9]|[12][0-9]|30)|sp|wsp|xzr|wzr)\b", operand)


def w_of(register: str) -> str:
    if register == "sp":
        raise RebaseError("sp has no 32-bit form here")
    return "w" + register[1:]


def immediate_value(text: str):
    text = text.strip()
    if not text.startswith("#"):
        return None
    try:
        return int(text[1:], 0)
    except ValueError:
        return None


def add_immediate(register: str, value: int):
    if value >= 0:
        return f"\tadd {register}, {register}, #{value}"
    return f"\tsub {register}, {register}, #{-value}"


class Rebaser:
    def __init__(self, lines):
        self.lines = lines
        self.out = []

    def emit(self, text: str):
        self.out.append(text)

    def check_reserved(self, operands, line):
        for operand in operands:
            for token in register_tokens(operand):
                if token in RESERVED:
                    raise RebaseError("the guest uses a reserved register: " + line)

    # ---------- loads and stores

    def memory(self, op, operands, index, line):
        memory_operand = operands[index]
        pre_index = memory_operand.endswith("!")
        inner = memory_operand.rstrip("!").strip()
        if not (inner.startswith("[") and inner.endswith("]")):
            raise RebaseError("unexpected memory operand: " + line)
        parts = [p.strip() for p in inner[1:-1].split(",")]
        base = parts[0]
        post = operands[index + 1:] if index + 1 < len(operands) else []
        data = operands[:index]

        if base in ("sp", "x29"):
            # the stack (frame records, spills) is left as it is
            self.emit(line)
            return
        if not GPR64.match(base):
            raise RebaseError("unexpected base register: " + line)
        for operand in data:
            for token in register_tokens(operand):
                if token in HOST_FORM:
                    # a host address stored into guest memory, or loaded
                    # from it into sp or x29
                    raise RebaseError("host-form register through a guest pointer: " + line)
        offset = parts[1:]
        if offset and not (offset[0].startswith("#") or offset[0].startswith(":")):
            # register offset: [xN, Rm{, extend/shift}]
            if pre_index or post:
                raise RebaseError("writeback with a register offset: " + line)
            index_register = offset[0]
            if index_register in ("xzr", "wzr"):
                # (no index at all)
                self.emit(f"\tadd {SCRATCH}, {BASE}, {w_of(base)}, uxtw")
                self.emit(f"\t{op} " + ", ".join(data + [f"[{SCRATCH}]"]))
                return
            if not (GPR64.match(index_register) or GPR32.match(index_register)):
                raise RebaseError("unexpected index register: " + line)
            shift = 0
            if len(offset) > 1:
                m = re.match(r"^(lsl|uxtw|sxtw|sxtx|uxtx)(?:\s+#(\d+))?$", offset[1])
                if not m:
                    raise RebaseError("unexpected extend: " + line)
                shift = int(m.group(2) or 0)
            index_w = "w" + index_register[1:]
            self.emit(f"\tadd {SCRATCH_W}, {w_of(base)}, {index_w}" + (f", lsl #{shift}" if shift else ""))
            new_operands = data + [f"[{BASE}, {SCRATCH_W}, uxtw]"]
            self.emit(f"\t{op} " + ", ".join(new_operands))
            return

        if pre_index:
            value = immediate_value(offset[0]) if offset else 0
            if value is None:
                raise RebaseError("unexpected pre-index offset: " + line)
            if value:
                self.emit(add_immediate(base, value))
            self.emit(f"\tadd {SCRATCH}, {BASE}, {w_of(base)}, uxtw")
            self.emit(f"\t{op} " + ", ".join(data + [f"[{SCRATCH}]"]))
            return
        self.emit(f"\tadd {SCRATCH}, {BASE}, {w_of(base)}, uxtw")
        new_memory = "[" + ", ".join([SCRATCH] + offset) + "]"
        self.emit(f"\t{op} " + ", ".join(data + [new_memory]))
        if post:
            if len(post) != 1:
                raise RebaseError("unexpected post-index: " + line)
            step = post[0]
            value = immediate_value(step)
            if value is not None:
                if value:
                    self.emit(add_immediate(base, value))
            elif GPR64.match(step):
                self.emit(f"\tadd {base}, {base}, {step}")
            else:
                raise RebaseError("unexpected post-index step: " + line)

    # ---------- everything else

    def instruction(self, line):
        body = line.strip()
        parts = body.split(None, 1)
        op = parts[0].lower()
        operands = split_operands(parts[1]) if len(parts) > 1 else []
        self.check_reserved(operands, line)

        for i, operand in enumerate(operands):
            if operand.startswith("["):
                self.memory(op, operands, i, line)
                return

        if op in ("br", "blr", "braaz", "blraaz"):
            target = operands[0]
            if not GPR64.match(target):
                raise RebaseError("unexpected branch target: " + line)
            self.emit(f"\tadd {SCRATCH}, {BASE}, {w_of(target)}, uxtw")
            self.emit(f"\t{op} {SCRATCH}")
            return
        if op == "ret":
            if operands and operands[0] != "x30":
                target = operands[0]
                self.emit(f"\tadd {SCRATCH}, {BASE}, {w_of(target)}, uxtw")
                self.emit(f"\tret {SCRATCH}")
                return
            self.emit(line)
            return
        if op in ("adrp", "adr"):
            destination = operands[0]
            if not GPR64.match(destination) or destination in HOST_FORM or destination in ("x27", "x28"):
                raise RebaseError("unexpected destination: " + line)
            self.emit(line)
            self.emit(f"\tmov {w_of(destination)}, {w_of(destination)}")
            return

        tokens = [register_tokens(operand) for operand in operands]
        flat = [t for group in tokens for t in group]
        if not any(t in HOST_FORM or t == "wsp" for t in flat):
            self.emit(line)
            return
        if op in NO_DESTINATION or not operands:
            if op in ("cmp", "cmn") and operands and operands[0] == "sp" and immediate_value(operands[1]) is not None:
                self.emit(line)
                return
            raise RebaseError("host-form register compared or branched on: " + line)

        destination = operands[0]
        sources = operands[1:]
        if destination in ("sp", "x29"):
            # sp/x29 from another register: that register is a guest pointer
            if sources:
                first = sources[0]
                if GPR64.match(first) and first not in HOST_FORM:
                    self.emit(f"\tadd {SCRATCH}, {BASE}, {w_of(first)}, uxtw")
                    self.emit(f"\t{op} " + ", ".join([destination, SCRATCH] + sources[1:]))
                    return
                for operand in sources[1:]:
                    for token in register_tokens(operand):
                        if token == "x29":
                            raise RebaseError("unexpected stack computation: " + line)
            self.emit(line)
            return
        if GPR64.match(destination):
            self.emit(line)
            self.emit(f"\tmov {w_of(destination)}, {w_of(destination)}")
            return
        if GPR32.match(destination) or destination in ("wsp",):
            # a 32-bit result holds no window base
            self.emit(line)
            return
        raise RebaseError("unexpected use of a host-form register: " + line)

    def run(self):
        for number, raw in enumerate(self.lines):
            line = raw.rstrip()
            stripped = line.strip()
            try:
                # (android_asm_convert.py puts labels on lines of their own)
                if not stripped or stripped.startswith(("//", "#", ".")) or stripped.endswith(":"):
                    self.emit(line)
                    continue
                self.instruction(line)
            except RebaseError as error:
                raise RebaseError(f"line {number + 1}: {error}") from None
        return "\n".join(self.out) + "\n"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    with open(sys.argv[1], "r", encoding="utf-8", errors="surrogateescape") as f:
        lines = f.read().split("\n")
    try:
        result = Rebaser(lines).run()
    except RebaseError as error:
        print(f"{sys.argv[1]}: {error}", file=sys.stderr)
        sys.exit(1)
    with open(sys.argv[2], "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(result)


if __name__ == "__main__":
    main()
