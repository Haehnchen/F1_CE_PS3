"""Extract and disassemble an SPU ELF embedded in the PPU executable.

usage: python -I tools/spu_elf.py <vaddr hex> [out.txt]
Prints the ELF header and program headers, then a disassembly of the loadable segments with
LS addresses (one instruction per line; data words are disassembled too).
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import ps3elf  # noqa: E402
import spu  # noqa: E402


def load_spu_elf(elf, va):
    o = elf.v2o(va)
    data = elf.data[o:o + 0x100000]
    assert data[:4] == b"\x7fELF", "no ELF at %#x" % va
    e_entry, e_phoff = struct.unpack(">II", data[0x18:0x20])
    e_phentsize, e_phnum = struct.unpack(">HH", data[0x2A:0x2E])
    segs = []
    for i in range(e_phnum):
        p_type, p_off, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = struct.unpack(
            ">8I", data[e_phoff + i * e_phentsize:e_phoff + i * e_phentsize + 32])
        segs.append((p_type, p_off, p_vaddr, p_filesz, p_memsz, p_flags))
    return data, e_entry, segs


def main():
    va = int(sys.argv[1], 16)
    elf = ps3elf.load(Path(__file__).parents[1] / "input" / "EBOOT.elf")
    data, entry, segs = load_spu_elf(elf, va)
    out = [f"SPU ELF at {va:#x}: entry {entry:#x}"]
    for t, off, vaddr, fsz, msz, fl in segs:
        out.append(f"  type {t:#x} off {off:#x} ls {vaddr:#07x} filesz {fsz:#x} memsz {msz:#x} flags {fl:#x}")
    for t, off, vaddr, fsz, msz, fl in segs:
        if t != 1 or not fsz:
            continue
        out.append(f"\n--- segment ls {vaddr:#x}..{vaddr + fsz:#x} flags {fl:#x}")
        for i in range(0, fsz - 3, 4):
            w = struct.unpack(">I", data[off + i:off + i + 4])[0]
            out.append(f"{vaddr + i:05x}: {w:08x}  {spu.disasm(w, vaddr + i)}")
    text = "\n".join(out)
    if len(sys.argv) > 2:
        Path(sys.argv[2]).write_text(text)
        print("\n".join(out[:len(segs) + 1]))
    else:
        print(text)


if __name__ == "__main__":
    main()
