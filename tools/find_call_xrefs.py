from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_64, Cs


TEXT_BASE = 0x1000
TARGETS = (0x0183C010, 0x021F2A30, 0x0118F9E0)
text = Path(r"E:\trigger_ac7vr\evidence\Ace7Game.text.runtime.bin").read_bytes()
md = Cs(CS_ARCH_X86, CS_MODE_64)

for target in TARGETS:
    hits = []
    for i in range(len(text) - 5):
        if text[i] != 0xE8:
            continue
        address = TEXT_BASE + i
        rel = int.from_bytes(text[i + 1 : i + 5], "little", signed=True)
        if address + 5 + rel == target:
            hits.append(address)

    print(f"TARGET {target:08X} CALL_XREFS {len(hits)}")
    for address in hits:
        start = max(TEXT_BASE, address - 0x90)
        end = min(TEXT_BASE + len(text), address + 0xA0)
        print(f"CALLSITE {address:08X}")
        for insn in md.disasm(text[start - TEXT_BASE : end - TEXT_BASE], start):
            marker = "  <==" if insn.address == address else ""
            print(f"{insn.address:08X}: {insn.mnemonic:8} {insn.op_str}{marker}")
