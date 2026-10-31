"""Minimal loader for the decrypted PS3 PPU executable (ELF64 big-endian, CellOS ABI)."""
import hashlib
import struct
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_ELF = Path(__file__).resolve().parents[2] / \
    "Formula One - Championship Edition (Europe) (En,Fr,De,Es,It,Pt,Fi)" / "PS3_GAME" / "USRDIR" / "EBOOT.elf"

# Suffix used by Sony to derive function NIDs: NID = le32(sha1(name + suffix)[0:4])
NID_SUFFIX = bytes.fromhex("6759659904250490566427499489741A")


def nid_of(name: str) -> int:
    return struct.unpack("<I", hashlib.sha1(name.encode() + NID_SUFFIX).digest()[:4])[0]


@dataclass
class Segment:
    vaddr: int
    offset: int
    filesz: int
    memsz: int
    flags: int


@dataclass
class Import:
    library: str
    nid: int
    stub_slot: int          # guest address of the u32 slot the loader patches
    name: str | None = None


@dataclass
class PS3Elf:
    data: bytes
    entry: int
    segments: list[Segment]
    sections: list[tuple]
    imports: list[Import] = field(default_factory=list)
    toc: int = 0

    # --- memory access -------------------------------------------------
    def v2o(self, va: int) -> int | None:
        for s in self.segments:
            if s.vaddr <= va < s.vaddr + s.filesz:
                return s.offset + va - s.vaddr
        return None

    def u32(self, va: int) -> int:
        o = self.v2o(va)
        return struct.unpack(">I", self.data[o:o + 4])[0]

    def cstr(self, va: int) -> str:
        o = self.v2o(va)
        return self.data[o:self.data.index(b"\0", o)].decode("latin-1")

    def is_code(self, va: int) -> bool:
        return any(s.vaddr <= va < s.vaddr + s.filesz and s.flags & 1 for s in self.segments)

    # --- structures ----------------------------------------------------
    def section(self, idx: int) -> tuple[int, int, int]:
        """(addr, offset, size) of section idx."""
        s = self.sections[idx]
        return s[3], s[4], s[5]

    def opd_entries(self) -> list[tuple[int, int]]:
        """Function descriptors (entry, toc) from the .opd section (8 bytes each on PS3)."""
        addr, _, size = self.section(OPD_SECTION)
        out = []
        for a in range(addr, addr + size, 8):
            out.append((self.u32(a), self.u32(a + 4)))
        return out


OPD_SECTION = 20   # section 21 is the TOC (r2 = 0x70f138 points 0x8000 into it)


def load(path: Path = DEFAULT_ELF) -> PS3Elf:
    d = Path(path).read_bytes()
    assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 2, "not an ELF64 BE file"
    entry = struct.unpack(">Q", d[24:32])[0]
    phoff, shoff = struct.unpack(">QQ", d[32:48])
    phnum = struct.unpack(">H", d[56:58])[0]
    shnum = struct.unpack(">H", d[60:62])[0]
    segs = []
    proc_param = None
    for i in range(phnum):
        t, f, off, va, _pa, fs, ms, _al = struct.unpack(">IIQQQQQQ", d[phoff + i * 56:phoff + i * 56 + 56])
        if t == 1 and ms:
            segs.append(Segment(va, off, fs, ms, f))
        elif t == 0x60000002:
            proc_param = va
    secs = [struct.unpack(">IIQQQQIIQQ", d[shoff + i * 64:shoff + i * 64 + 64]) for i in range(shnum)]
    elf = PS3Elf(d, entry, segs, secs)
    # entry is a function descriptor
    elf.toc = elf.u32(entry + 4)
    _load_imports(elf, proc_param)
    return elf


def _load_imports(elf: PS3Elf, prx_param: int) -> None:
    o = elf.v2o(prx_param)
    f = struct.unpack(">8I", elf.data[o:o + 32])
    stub_start, stub_end = f[6], f[7]
    names = _known_names()
    a = stub_start
    while a < stub_end:
        o = elf.v2o(a)
        size = elf.data[o]
        nfunc = struct.unpack(">H", elf.data[o + 6:o + 8])[0]
        lib = elf.cstr(elf.u32(a + 0x10))
        nid_tab, addr_tab = elf.u32(a + 0x14), elf.u32(a + 0x18)
        for i in range(nfunc):
            nid = elf.u32(nid_tab + 4 * i)
            elf.imports.append(Import(lib, nid, addr_tab + 4 * i, names.get(nid)))
        a += size


def _known_names() -> dict[int, str]:
    p = Path(__file__).with_name("nid_names.txt")
    if not p.exists():
        return {}
    return {nid_of(n): n for n in (l.strip() for l in p.read_text().splitlines()) if n and not n.startswith("#")}


if __name__ == "__main__":
    e = load()
    print(f"entry descriptor {e.entry:#x}  toc {e.toc:#x}")
    print(f"opd entries: {len(e.opd_entries())}")
    unknown = 0
    for imp in e.imports:
        unknown += imp.name is None
        print(f"{imp.library:14} {imp.nid:08x} slot={imp.stub_slot:#x} -> {e.u32(imp.stub_slot):#x}  {imp.name or '?'}")
    print(f"{len(e.imports)} imports, {unknown} unresolved names")
