"""Function discovery for the PPU executable.

Function starts are the union of:
  * code addresses of every .opd descriptor (all address-taken / exported functions)
  * targets of direct calls (bl)
  * the ELF entry descriptor
A function extends up to the next start. Stub thunks for imports live in their own section
and are recognised separately (one 32-byte stub per import).
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import ps3elf  # noqa: E402
import ppc  # noqa: E402

CODE_SECTIONS = (1, 2, 3)    # .init, .text, .fini (names are stripped; identified by layout)
STUB_SECTION = 4             # .sceStub.text
STUB_SIZE = 32


def words(elf, idx):
    addr, off, size = elf.section(idx)
    for i in range(0, size, 4):
        yield addr + i, struct.unpack(">I", elf.data[off + i:off + i + 4])[0]


def discover(elf):
    code_ranges = [elf.section(i) for i in CODE_SECTIONS]
    code_ranges = [(a, a + s) for a, _, s in code_ranges]
    stub_addr, _, stub_size = elf.section(STUB_SECTION)

    def in_code(a):
        return any(lo <= a < hi for lo, hi in code_ranges)

    starts = {}
    for entry, _toc in elf.opd_entries():
        if in_code(entry):
            starts[entry] = "opd"
    for idx in CODE_SECTIONS:
        for a, w in words(elf, idx):
            if w >> 26 == 18 and w & 1:              # bl / bla
                t = (ppc.LI(w) if not ppc.AA(w) else ppc.LI(w) & 0xFFFFFFFF) + (0 if ppc.AA(w) else a)
                t &= 0xFFFFFFFF
                if in_code(t) and t != a + 4:        # bl $+4 is the PIC "get PC" idiom, not a call
                    starts.setdefault(t, "call")
    starts.setdefault(elf.u32(elf.entry), "entry")

    # Imports: map each stub (32 bytes) to the import whose slot it references.
    stubs = {}
    slot_to_imp = {imp.stub_slot: imp for imp in elf.imports}
    for s in range(stub_addr, stub_addr + stub_size, STUB_SIZE):
        # stub: li r12,0 ; oris r12,r12,hi ; lwz r12,lo(r12) ; ... ; bctr
        hi = lo = None
        for k in range(0, STUB_SIZE, 4):
            w = elf.u32(s + k)
            m = ppc.decode(w)
            if m == "oris":
                hi = ppc.UIMM(w)
            elif m == "addis":
                hi = ppc.UIMM(w)
            elif m == "lwz" and hi is not None and lo is None:
                lo = ppc.SIMM(w)
        slot = ((hi << 16) + lo) & 0xFFFFFFFF if hi is not None and lo is not None else None
        stubs[s] = slot_to_imp.get(slot)

    ordered = sorted(starts)
    bounds = []
    for i, a in enumerate(ordered):
        end = ordered[i + 1] if i + 1 < len(ordered) else next(hi for lo, hi in code_ranges if lo <= a < hi)
        # never cross a section boundary
        sec_end = next(hi for lo, hi in code_ranges if lo <= a < hi)
        bounds.append((a, min(end, sec_end), starts[a]))
    return bounds, stubs


if __name__ == "__main__":
    elf = ps3elf.load(Path(__file__).parents[1] / "input" / "EBOOT.elf")
    funcs, stubs = discover(elf)
    kinds = {}
    for _, _, k in funcs:
        kinds[k] = kinds.get(k, 0) + 1
    print(f"{len(funcs)} functions {kinds}")
    print(f"{len(stubs)} import stubs, {sum(v is None for v in stubs.values())} unmatched")
    sizes = sorted(e - s for s, e, _ in funcs)
    print("size min/median/max:", sizes[0], sizes[len(sizes) // 2], sizes[-1])
