#!/usr/bin/env python3
"""Post-link sanity check: fail the build if any two WRAM sections overlap.

Why this exists: the C compiler places all globals in one .bss section starting at $7E:2000,
while pvsneslib's own RAM (console text buffer, OAM shadow buffer, ...) sits at fixed
addresses from $7E:8000 upward.  wla-link does NOT complain when a large .bss runs into the
library's sections -- the ROM builds fine and then corrupts library state at run time.  That
leaves about 24 KB of $7E for C globals; anything bigger belongs in bank $7F (see canvas.inc).

Usage: check_ram.py pizzasnake.sym
"""
import re
import sys

sym = sys.argv[1] if len(sys.argv) > 1 else "pizzasnake.sym"
starts, ends = [], []
for line in open(sym):
    m = re.match(r"^([0-9a-fA-F]{8})\s+SECTION(START|END)_(\S+)$", line.strip())
    if not m:
        continue
    addr = int(m.group(1), 16)
    # WRAM is $7E:0000-$7F:FFFF; $0000-$1FFF of bank 0 is a mirror of the start of $7E.
    if addr < 0x2000:
        wram = addr
    elif 0x7E0000 <= addr < 0x800000:
        wram = addr - 0x7E0000
    else:
        continue  # ROM section
    (starts if m.group(2) == "START" else ends).append((wram, m.group(3)))

# Pair each section's START with its END.  Several sections can share a name (there are two
# ".bss"), but sections of one name never nest, so the k-th START and the k-th END (each in
# address order) belong together.
sections = []
for name in {n for _, n in starts}:
    ss = sorted(a for a, n in starts if n == name)
    ee = sorted(a for a, n in ends if n == name)
    for s_addr, e_addr in zip(ss, ee):
        if e_addr > s_addr:
            sections.append((s_addr, e_addr, name))
sections.sort()

bad = False
for i, (a0, a1, n0) in enumerate(sections):
    for (b0, b1, n1) in sections[i + 1:]:
        if b0 >= a1:
            break
        print("RAM OVERLAP: %s (%05X-%05X) and %s (%05X-%05X)" % (n0, a0, a1 - 1, n1, b0, b1 - 1))
        bad = True
if bad:
    print("check_ram: WRAM sections overlap -- the ROM would corrupt memory at run time.")
    print("           Move big arrays to bank $7F (see canvas.inc) or shrink them.")
    sys.exit(1)
top = max((a1 for a0, a1, n in sections if n == ".bss"), default=0)
print("check_ram: ok (%d WRAM sections, no overlaps; C .bss ends at $7E:%04X, library RAM starts at $7E:8000)"
      % (len(sections), top))
