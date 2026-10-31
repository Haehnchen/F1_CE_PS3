"""PowerPC 64 (Cell PPU) instruction decoder: maps a 32-bit word to a mnemonic.

Field extraction helpers follow IBM bit numbering converted to shifts
(bit 0 = MSB). The translator reads operands straight from the word.
"""

# ---------------------------------------------------------------- fields
def OPCD(w): return w >> 26
def RD(w): return (w >> 21) & 31
def RS(w): return (w >> 21) & 31
def RA(w): return (w >> 16) & 31
def RB(w): return (w >> 11) & 31
def RC(w): return (w >> 6) & 31
def SIMM(w): return ((w & 0xFFFF) ^ 0x8000) - 0x8000
def UIMM(w): return w & 0xFFFF
def DS(w): return ((w & 0xFFFC) ^ 0x8000) - 0x8000
def Rc(w): return w & 1
def OE(w): return (w >> 10) & 1
def BO(w): return (w >> 21) & 31
def BI(w): return (w >> 16) & 31
def BD(w): return ((w & 0xFFFC) ^ 0x8000) - 0x8000
def LI(w): return ((w & 0x3FFFFFC) ^ 0x2000000) - 0x2000000
def AA(w): return (w >> 1) & 1
def LK(w): return w & 1
def CRFD(w): return (w >> 23) & 7
def CRFS(w): return (w >> 18) & 7
def L(w): return (w >> 21) & 1
def SH(w): return (w >> 11) & 31
def MB(w): return (w >> 6) & 31
def ME(w): return (w >> 1) & 31
def SH64(w): return ((w >> 11) & 31) | (((w >> 1) & 1) << 5)
def MB64(w): return ((w >> 6) & 31) | (((w >> 5) & 1) << 5)   # mb5 || mb0:4 encoded as low5|hi1
def SPR(w): return ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
def CRM(w): return (w >> 12) & 0xFF
def VD(w): return (w >> 21) & 31
def VA(w): return (w >> 16) & 31
def VB(w): return (w >> 11) & 31
def VC(w): return (w >> 6) & 31
def VSIMM(w): return (((w >> 16) & 31) ^ 16) - 16
def VUIMM(w): return (w >> 16) & 31
def VSH(w): return (w >> 6) & 15
def FM(w): return (w >> 17) & 0xFF


# ---------------------------------------------------------------- tables
PRIMARY = {
    2: "tdi", 3: "twi", 7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.",
    14: "addi", 15: "addis", 16: "bc", 17: "sc", 18: "b", 20: "rlwimi", 21: "rlwinm", 23: "rlwnm",
    24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis.",
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
    40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
    48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
}

OP19 = {0: "mcrf", 16: "bclr", 18: "rfid", 33: "crnor", 129: "crandc", 150: "isync", 193: "crxor",
        225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror", 528: "bcctr"}

OP30 = {0: "rldicl", 1: "rldicr", 2: "rldic", 3: "rldimi"}      # bits 27-29
OP30_MDS = {8: "rldcl", 9: "rldcr"}                               # bits 27-30

OP58 = {0: "ld", 1: "ldu", 2: "lwa"}
OP62 = {0: "std", 1: "stdu"}

OP59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls", 26: "frsqrtes",
        28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}

OP63_A = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 24: "fre", 25: "fmul",
          26: "frsqrte", 28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}
OP63_X = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1", 40: "fneg",
          64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs", 264: "fabs", 583: "mffs",
          711: "mtfsf", 814: "fctid", 815: "fctidz", 846: "fcfid"}

OP31 = {
    0: "cmp", 4: "tw", 6: "lvsl", 7: "lvebx", 19: "mfcr", 20: "lwarx", 21: "ldx", 23: "lwzx",
    24: "slw", 26: "cntlzw", 27: "sld", 28: "and", 32: "cmpl", 38: "lvsr", 39: "lvehx", 53: "ldux",
    54: "dcbst", 55: "lwzux", 58: "cntlzd", 60: "andc", 68: "td", 71: "lvewx", 83: "mfmsr", 84: "ldarx",
    86: "dcbf", 87: "lbzx", 103: "lvx", 119: "lbzux", 124: "nor", 135: "stvebx", 144: "mtcrf",
    146: "mtmsr", 149: "stdx", 150: "stwcx.", 151: "stwx", 167: "stvehx", 178: "mtmsrd", 181: "stdux",
    183: "stwux", 199: "stvewx", 214: "stdcx.", 215: "stbx", 231: "stvx", 246: "dcbtst", 247: "stbux",
    278: "dcbt", 279: "lhzx", 284: "eqv", 310: "eciwx", 311: "lhzux", 316: "xor", 339: "mfspr",
    341: "lwax", 342: "dst", 343: "lhax", 359: "lvxl", 371: "mftb", 373: "lwaux", 374: "dstst",
    375: "lhaux", 407: "sthx", 412: "orc", 438: "ecowx", 439: "sthux", 444: "or", 467: "mtspr",
    470: "dcbi", 476: "nand", 487: "stvxl", 519: "lvlx", 532: "ldbrx", 534: "lwbrx", 535: "lfsx",
    536: "srw", 539: "srd", 551: "lvrx", 567: "lfsux", 598: "sync", 599: "lfdx", 631: "lfdux",
    647: "stvlx", 660: "stdbrx", 662: "stwbrx", 663: "stfsx", 679: "stvrx", 695: "stfsux", 727: "stfdx",
    759: "stfdux", 775: "lvlxl", 790: "lhbrx", 792: "sraw", 794: "srad", 807: "lvrxl", 822: "dss",
    824: "srawi", 854: "eieio", 903: "stvlxl", 918: "sthbrx", 922: "extsh", 935: "stvrxl", 954: "extsb",
    982: "icbi", 983: "stfiwx", 986: "extsw", 1014: "dcbz",
}
OP31_XO = {8: "subfc", 9: "mulhdu", 10: "addc", 11: "mulhwu", 40: "subf", 73: "mulhd", 75: "mulhw",
           104: "neg", 136: "subfe", 138: "adde", 200: "subfze", 202: "addze", 232: "subfme",
           233: "mulld", 234: "addme", 235: "mullw", 266: "add", 457: "divdu", 459: "divwu",
           489: "divd", 491: "divw"}

VA_FORM = {32: "vmhaddshs", 33: "vmhraddshs", 34: "vmladduhm", 36: "vmsumubm", 37: "vmsummbm",
           38: "vmsumuhm", 39: "vmsumuhs", 40: "vmsumshm", 41: "vmsumshs", 42: "vsel", 43: "vperm",
           44: "vsldoi", 46: "vmaddfp", 47: "vnmsubfp"}
VC_FORM = {6: "vcmpequb", 70: "vcmpequh", 134: "vcmpequw", 198: "vcmpeqfp", 454: "vcmpgefp",
           518: "vcmpgtub", 582: "vcmpgtuh", 646: "vcmpgtuw", 710: "vcmpgtfp", 774: "vcmpgtsb",
           838: "vcmpgtsh", 902: "vcmpgtsw", 966: "vcmpbfp"}
VX_FORM = {
    0: "vaddubm", 64: "vadduhm", 128: "vadduwm", 384: "vaddcuw", 512: "vaddubs", 576: "vadduhs",
    640: "vadduws", 768: "vaddsbs", 832: "vaddshs", 896: "vaddsws", 1024: "vsububm", 1088: "vsubuhm",
    1152: "vsubuwm", 1408: "vsubcuw", 1536: "vsububs", 1600: "vsubuhs", 1664: "vsubuws", 1792: "vsubsbs",
    1856: "vsubshs", 1920: "vsubsws", 2: "vmaxub", 66: "vmaxuh", 130: "vmaxuw", 258: "vmaxsb",
    322: "vmaxsh", 386: "vmaxsw", 514: "vminub", 578: "vminuh", 642: "vminuw", 770: "vminsb",
    834: "vminsh", 898: "vminsw", 1026: "vavgub", 1090: "vavguh", 1154: "vavguw", 1282: "vavgsb",
    1346: "vavgsh", 1410: "vavgsw", 4: "vrlb", 68: "vrlh", 132: "vrlw", 260: "vslb", 324: "vslh",
    388: "vslw", 452: "vsl", 516: "vsrb", 580: "vsrh", 644: "vsrw", 708: "vsr", 772: "vsrab",
    836: "vsrah", 900: "vsraw", 1028: "vand", 1092: "vandc", 1156: "vor", 1220: "vxor", 1284: "vnor",
    1540: "mfvscr", 1604: "mtvscr", 8: "vmuloub", 72: "vmulouh", 264: "vmulosb", 328: "vmulosh",
    520: "vmuleub", 584: "vmuleuh", 776: "vmulesb", 840: "vmulesh", 1544: "vsum4ubs", 1800: "vsum4sbs",
    1608: "vsum4shs", 1672: "vsum2sws", 1928: "vsumsws", 10: "vaddfp", 74: "vsubfp", 266: "vrefp",
    330: "vrsqrtefp", 394: "vexptefp", 458: "vlogefp", 522: "vrfin", 586: "vrfiz", 650: "vrfip",
    714: "vrfim", 778: "vcfux", 842: "vcfsx", 906: "vctuxs", 970: "vctsxs", 1034: "vmaxfp",
    1098: "vminfp", 12: "vmrghb", 76: "vmrghh", 140: "vmrghw", 268: "vmrglb", 332: "vmrglh",
    396: "vmrglw", 524: "vspltb", 588: "vsplth", 652: "vspltw", 780: "vspltisb", 844: "vspltish",
    908: "vspltisw", 1036: "vslo", 1100: "vsro", 14: "vpkuhum", 78: "vpkuwum", 142: "vpkuhus",
    206: "vpkuwus", 270: "vpkshus", 334: "vpkswus", 398: "vpkshss", 462: "vpkswss", 526: "vupkhsb",
    590: "vupkhsh", 654: "vupklsb", 718: "vupklsh", 782: "vpkpx", 846: "vupkhpx", 974: "vupklpx",
}


def decode(w: int) -> str | None:
    """Return the base mnemonic for instruction word w, or None if invalid/unknown."""
    op = w >> 26
    if op in PRIMARY:
        return PRIMARY[op]
    if op == 4:
        va = w & 0x3F
        if va in VA_FORM:
            return VA_FORM[va]
        vc = w & 0x3FF
        if vc in VC_FORM:
            return VC_FORM[vc] + ("." if w & 0x400 else "")
        return VX_FORM.get(w & 0x7FF)
    if op == 19:
        return OP19.get((w >> 1) & 0x3FF)
    if op == 30:
        x = (w >> 2) & 7
        if x in OP30:
            return OP30[x]
        return OP30_MDS.get((w >> 1) & 0xF)
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in OP31:
            return OP31[xo]
        if xo >> 1 == 413:
            return "sradi"
        return OP31_XO.get(xo & 0x1FF)
    if op == 58:
        return OP58.get(w & 3)
    if op == 62:
        return OP62.get(w & 3)
    if op == 59:
        return OP59.get((w >> 1) & 0x1F)
    if op == 63:
        a = (w >> 1) & 0x1F
        if a in OP63_A:
            return OP63_A[a]
        return OP63_X.get((w >> 1) & 0x3FF)
    return None
