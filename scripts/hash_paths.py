"""Stream the community hash dictionaries and collect paths of interest.

The dictionaries are large (~220 MB); loading them into a dict would waste RAM.
We only need the *paths* that look like VFX, then let lolview test them by hash.
"""
import glob
import re
import sys

HASH_RE = re.compile(r"^[0-9a-fA-F]{16}$")
CHAMPION = sys.argv[1] if len(sys.argv) > 1 else "Ahri"

champion_re = re.compile(CHAMPION, re.IGNORECASE)
vfx_re = re.compile(r"particle|preload|troy|vfx|effect", re.IGNORECASE)

matches = set()
total_lines = 0

for path in sorted(glob.glob(r"_local\hashes\hashes.game.txt.*")):
    try:
        handle = open(path, "r", encoding="utf-8", errors="replace")
    except OSError as error:
        print(f"skipping {path}: {error}")
        continue
    with handle:
        for line in handle:
            total_lines += 1
            parts = line.split()
            if len(parts) < 2:
                continue
            candidate = None
            for token in parts[:2]:
                if not HASH_RE.match(token):
                    candidate = token
                    break
            if candidate is None:
                continue
            if champion_re.search(candidate) and vfx_re.search(candidate):
                matches.add(candidate)

ordered = sorted(matches)
out = r"_local\hash_vfx_candidates.txt"
with open(out, "w", encoding="utf-8") as handle:
    handle.write("\n".join(ordered))

print(f"{total_lines} lines scanned, {len(ordered)} matching paths -> {out}")
for item in ordered[:40]:
    print("  ", item)
