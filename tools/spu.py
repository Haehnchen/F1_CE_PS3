"""Cell SPU instruction decoder / disassembler."""

RRR = {0x8: "selb", 0xB: "shufb", 0xC: "mpya", 0xD: "fnms", 0xE: "fma", 0xF: "fms"}           # 4-bit
RI18 = {0x21: "ila", 0x08: "hbra", 0x09: "hbrr"}                                              # 7-bit
RI10 = {0x34: "lqd", 0x24: "stqd", 0x1C: "ai", 0x1D: "ahi", 0x0C: "sfi", 0x0D: "sfhi",         # 8-bit
        0x14: "andi", 0x15: "andhi", 0x16: "andbi", 0x04: "ori", 0x05: "orhi", 0x06: "orbi",
        0x44: "xori", 0x45: "xorhi", 0x46: "xorbi", 0x74: "mpyi", 0x75: "mpyui", 0x7C: "ceqi",
        0x7D: "ceqhi", 0x7E: "ceqbi", 0x4C: "cgti", 0x4D: "cgthi", 0x4E: "cgtbi", 0x5C: "clgti",
        0x5D: "clgthi", 0x5E: "clgtbi", 0x7F: "heqi", 0x4F: "hgti", 0x5F: "hlgti"}
RI16 = {0x064: "br", 0x066: "brsl", 0x060: "bra", 0x062: "brasl", 0x042: "brnz", 0x040: "brz",  # 9-bit
        0x046: "brhnz", 0x044: "brhz", 0x061: "lqa", 0x067: "lqr", 0x041: "stqa", 0x047: "stqr",
        0x083: "ilh", 0x082: "ilhu", 0x081: "il", 0x0C1: "iohl", 0x065: "fsmbi"}
RI8 = {0x1D8: "cflts", 0x1D9: "cfltu", 0x1DA: "csflt", 0x1DB: "cuflt"}                          # 10-bit
RR = {                                                                                          # 11-bit
    0x000: "stop", 0x001: "lnop", 0x002: "sync", 0x003: "dsync", 0x00C: "mfspr", 0x00D: "rdch",
    0x00F: "rchcnt", 0x040: "sf", 0x041: "or", 0x042: "bg", 0x048: "sfh", 0x049: "nor", 0x053: "absdb",
    0x058: "rot", 0x059: "rotm", 0x05A: "rotma", 0x05B: "shl", 0x05C: "roth", 0x05D: "rothm",
    0x05E: "rotmah", 0x05F: "shlh", 0x078: "roti", 0x079: "rotmi", 0x07A: "rotmai", 0x07B: "shli",
    0x07C: "rothi", 0x07D: "rothmi", 0x07E: "rotmahi", 0x07F: "shlhi", 0x0C0: "a", 0x0C1: "and",
    0x0C2: "cg", 0x0C8: "ah", 0x0C9: "nand", 0x0D3: "avgb", 0x10C: "mtspr", 0x10D: "wrch",
    0x128: "biz", 0x129: "binz", 0x12A: "bihz", 0x12B: "bihnz", 0x140: "stopd", 0x144: "stqx",
    0x1A8: "bi", 0x1A9: "bisl", 0x1AA: "iret", 0x1AB: "bisled", 0x1AC: "hbr", 0x1B0: "gb", 0x1B1: "gbh",
    0x1B2: "gbb", 0x1B4: "fsm", 0x1B5: "fsmh", 0x1B6: "fsmb", 0x1B8: "frest", 0x1B9: "frsqest",
    0x1C4: "lqx", 0x1CC: "rotqbybi", 0x1CD: "rotqmbybi", 0x1CF: "shlqbybi", 0x1D4: "cbx", 0x1D5: "chx",
    0x1D6: "cwx", 0x1D7: "cdx", 0x1D8: "rotqbi", 0x1D9: "rotqmbi", 0x1DB: "shlqbi", 0x1DC: "rotqby",
    0x1DD: "rotqmby", 0x1DF: "shlqby", 0x1F0: "orx", 0x1F4: "cbd", 0x1F5: "chd", 0x1F6: "cwd", 0x1F7: "cdd",
    0x1F8: "rotqbii", 0x1F9: "rotqmbii", 0x1FB: "shlqbii", 0x1FC: "rotqbyi", 0x1FD: "rotqmbyi",
    0x1FF: "shlqbyi", 0x201: "nop", 0x240: "cgt", 0x241: "xor", 0x248: "cgth", 0x249: "eqv", 0x250: "cgtb",
    0x253: "sumb", 0x258: "hgt", 0x2A5: "clz", 0x2A6: "xswd", 0x2AE: "xshw", 0x2B4: "cntb", 0x2B6: "xsbh",
    0x2C0: "clgt", 0x2C1: "andc", 0x2C2: "fcgt", 0x2C3: "dfcgt", 0x2C4: "fa", 0x2C5: "fs", 0x2C6: "fm",
    0x2C8: "clgth", 0x2C9: "orc", 0x2CA: "fcmgt", 0x2CB: "dfcmgt", 0x2CC: "dfa", 0x2CD: "dfs", 0x2CE: "dfm",
    0x2D0: "clgtb", 0x2D8: "hlgt", 0x35C: "dfma", 0x35D: "dfms", 0x35E: "dfnms", 0x35F: "dfnma",
    0x3C0: "ceq", 0x3CE: "mpyhhu", 0x340: "addx", 0x341: "sfx", 0x342: "cgx", 0x343: "bgx", 0x346: "mpyhha",
    0x34E: "mpyhhau", 0x398: "fscrrd", 0x3B8: "fesd", 0x3B9: "frds", 0x3BA: "fscrwr", 0x3BF: "dftsv",
    0x3C2: "fceq", 0x3C3: "dfceq", 0x3C4: "mpy", 0x3C5: "mpyh", 0x3C6: "mpyhh", 0x3C7: "mpys", 0x3C8: "ceqh",
    0x3CA: "fcmeq", 0x3CB: "dfcmeq", 0x3CC: "mpyu", 0x3D0: "ceqb", 0x3D4: "fi", 0x3D8: "heq",
}
RI7_OPS = {"roti", "rotmi", "rotmai", "shli", "rothi", "rothmi", "rotmahi", "shlhi", "rotqbii", "rotqmbii",
           "shlqbii", "rotqbyi", "rotqmbyi", "shlqbyi", "cbd", "chd", "cwd", "cdd"}


def sext(v, bits):
    return ((v & ((1 << bits) - 1)) ^ (1 << (bits - 1))) - (1 << (bits - 1))


def decode(w):
    """Return (mnemonic, form) or (None, None)."""
    if (w >> 28) in RRR:
        return RRR[w >> 28], "RRR"
    if (w >> 25) in RI18:
        return RI18[w >> 25], "RI18"
    if (w >> 24) in RI10:
        return RI10[w >> 24], "RI10"
    if (w >> 23) in RI16:
        return RI16[w >> 23], "RI16"
    if (w >> 22) in RI8:
        return RI8[w >> 22], "RI8"
    if (w >> 21) in RR:
        m = RR[w >> 21]
        return m, "RI7" if m in RI7_OPS else "RR"
    return None, None


def disasm(w, pc):
    m, f = decode(w)
    rt, ra, rb, rc = w & 0x7F, (w >> 7) & 0x7F, (w >> 14) & 0x7F, (w >> 21) & 0x7F
    if m is None:
        return f".long 0x{w:08x}"
    if f == "RRR":
        return f"{m} r{rc},r{ra},r{rb},r{rt}"   # rt field position for RRR is bits 4-10 (rc in 25-31)
    if f == "RI18":
        return f"{m} r{rt},0x{(w >> 7) & 0x3FFFF:x}"
    if f == "RI10":
        i10 = sext(w >> 14, 10)
        if m in ("lqd", "stqd"):
            return f"{m} r{rt},{i10 * 16}(r{ra})"
        return f"{m} r{rt},r{ra},{i10}"
    if f == "RI16":
        i16 = sext(w >> 7, 16)
        if m in ("br", "brsl", "brnz", "brz", "brhnz", "brhz", "lqr", "stqr"):
            return f"{m} r{rt},0x{(pc + i16 * 4) & 0x3FFFF:x}"
        if m in ("bra", "brasl", "lqa", "stqa"):
            return f"{m} r{rt},0x{(i16 * 4) & 0x3FFFF:x}"
        return f"{m} r{rt},0x{(w >> 7) & 0xFFFF:x}"
    if f == "RI8":
        return f"{m} r{rt},r{ra},{173 - ((w >> 14) & 0xFF) if m in ('cflts', 'cfltu') else ((w >> 14) & 0xFF) - 155}"
    if f == "RI7":
        return f"{m} r{rt},r{ra},{sext(w >> 14, 7)}"
    if m in ("rdch", "rchcnt"):
        return f"{m} r{rt},ch{ra}"
    if m == "wrch":
        return f"{m} ch{ra},r{rt}"
    if m == "stop":
        return f"stop 0x{w & 0x3FFF:x}"
    return f"{m} r{rt},r{ra},r{rb}"


def RRR_fields(w):
    """RRR: rt (result) bits 25-31? -- IBM numbering: RT = bits 4-10, RB = 11-17, RA = 18-24, RC = 25-31."""
    return (w >> 21) & 0x7F, (w >> 7) & 0x7F, (w >> 14) & 0x7F, w & 0x7F   # rt, ra, rb, rc
