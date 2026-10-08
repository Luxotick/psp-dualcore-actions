"""Inspect PSP binary structure and ME arithmetic; never simulate hardware."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import subprocess


def command(*args):
    return subprocess.check_output([str(a) for a in args]).decode("utf-8", errors="replace")


def sections(data):
    offset = struct.unpack_from("<I", data, 32)[0]
    size, count, names_index = struct.unpack_from("<HHH", data, 46)
    headers = [struct.unpack_from("<10I", data, offset + i * size) for i in range(count)]
    name_header = headers[names_index]
    names = data[name_header[4]:name_header[4] + name_header[5]]
    result = {}
    for h in headers:
        name = names[h[0]:].split(b"\0", 1)[0].decode("ascii")
        result[name] = {"addr": h[3], "offset": h[4], "size": h[5], "align": h[8]}
    return result


def import_names(data, sec):
    stubs = sec[".lib.stub"]
    names = []
    cursor = stubs["offset"]
    while cursor < stubs["offset"] + stubs["size"]:
        name_addr = struct.unpack_from("<I", data, cursor)[0]
        words = data[cursor + 8]
        assert words >= 5, "Invalid PSP import descriptor"
        for s in sec.values():
            if s["addr"] <= name_addr < s["addr"] + s["size"]:
                off = s["offset"] + name_addr - s["addr"]
                names.append(data[off:].split(b"\0", 1)[0].decode("ascii"))
                break
        else: raise AssertionError("Import name outside ELF sections")
        cursor += words * 4
    return names


def verify(build, output):
    pbp = (build / "EBOOT.PBP").read_bytes()
    assert pbp[:4] == b"\x00PBP", "Invalid PBP magic"
    offsets = struct.unpack_from("<8I", pbp, 8)
    assert list(offsets) == sorted(offsets) and offsets[0] == 40
    assert offsets[-1] == len(pbp), "Unexpected PSAR/truncated PBP"
    assert pbp[offsets[0]:offsets[0] + 4] == b"\x00PSF"
    prx = pbp[offsets[6]:offsets[7]]
    assert prx == (build / "psp-dualcore-actions.prx").read_bytes()
    assert prx[:6] == b"\x7fELF\x01\x01", "Expected unsigned ELF32 little-endian PRX for CFW"
    elf_path = build / "psp-dualcore-actions"
    if not elf_path.exists(): elf_path = build / "psp-dualcore-actions.elf"
    elf = elf_path.read_bytes()
    assert elf[:6] == b"\x7fELF\x01\x01"
    assert struct.unpack_from("<H", elf, 18)[0] == 8, "Expected MIPS"
    sec = sections(elf)
    names = import_names(elf, sec)
    assert "kcall" in names, "Missing kernel bridge import"
    assert not any("ForKernel" in name or name.endswith("_driver") for name in names), names
    symbols = command("psp-nm", "-n", elf_path)
    bridge = re.search(r"^([0-9a-f]+) b shared_task$", symbols, re.M)
    assert bridge and int(bridge[1], 16) % 64 == 0, "Unaligned shared task"
    assert sec[".bss"]["align"] >= 64
    disasm = command("psp-objdump", "-d", "--disassemble=me_loop", elf_path)
    # The actual optimized ME code must load offset 12 (A), offset 16 (B),
    # add registers and store offset 20 (result). No immediate constant sum.
    assert re.search(r"\blw\s+[^\n]*12\(s0\)", disasm)
    assert re.search(r"\blw\s+[^\n]*16\(s0\)", disasm)
    assert re.search(r"\baddu\s+v1,v1,a0", disasm)
    assert re.search(r"\bsw\s+[^\n]*20\(s0\)", disasm)
    assert "syscall" not in disasm and not re.search(r"\bgp\b", disasm)
    all_disasm = command("psp-objdump", "-d", elf_path)
    assert not re.search(r"\bjal\s+[^\n]*<me_loop>", all_disasm), "Direct CPU call to ME task"
    output.mkdir(parents=True, exist_ok=True)
    (output / "me_loop.disassembly.txt").write_text(disasm, encoding="utf-8")
    report = {"status": "Static build checks passed; no ME/hardware execution performed",
              "pbp_bytes": len(pbp), "sha256": hashlib.sha256(pbp).hexdigest(),
              "imports": names, "shared_task_elf_offset": bridge[1],
              "checks": ["PBP/SFO/PRX integrity", "MIPS ELF32", "64-byte shared alignment",
                         "two operand loads and register addition", "no ME syscall or gp use",
                         "no direct CPU call to ME task", "no kernel-only application imports"]}
    (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(report["status"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    verify(args.build, args.output or args.build / "verification")
