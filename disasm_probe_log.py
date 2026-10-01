from pathlib import Path
import re

from capstone import Cs, CS_ARCH_X86, CS_MODE_64


LOG = Path(__file__).with_name("probe.log")
WANTED = {
    "ToggleVRTestMissionMenu_exec",
    "IsHMDConnected_native",
    "IsHMDEnabled_native",
}

text = LOG.read_text(encoding="utf-8", errors="replace")
sections = text.split("=== trigger_ac7vr probe ")
latest = sections[-1]

md = Cs(CS_ARCH_X86, CS_MODE_64)
for line in latest.splitlines():
    match = re.match(r"(\S+) @ ([0-9A-Fa-f]+) read=\d+/\d+: ([0-9A-Fa-f]+)$", line)
    if not match or match.group(1) not in WANTED:
        continue

    label, address_text, hex_bytes = match.groups()
    address = int(address_text, 16)
    blob = bytes.fromhex(hex_bytes)
    print(f"\n[{label}] @ 0x{address:x}")
    for insn in md.disasm(blob, address):
        print(f"0x{insn.address:x}: {insn.mnemonic:7s} {insn.op_str}")
        if insn.mnemonic == "ret":
            break
