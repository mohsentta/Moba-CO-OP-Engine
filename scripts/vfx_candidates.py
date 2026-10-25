"""Generate candidate VFX paths (.preload / .troy / particle textures)."""
import itertools
import sys

CHAMP = "Ahri"

dirs = [
    f"ASSETS/Characters/{CHAMP}/Skins/Base/Particles",
    f"ASSETS/Characters/{CHAMP}/Skins/Base/Particles/Textures",
    f"ASSETS/Characters/{CHAMP}/Particles",
    f"ASSETS/Characters/{CHAMP}/Skins/Base/VFX",
    f"ASSETS/Particles/{CHAMP}",
    f"ASSETS/Particles/Characters/{CHAMP}",
]

anims = [
    "Q_mis", "Q", "Q_mis_hit", "W", "W_mis", "E", "E_mis", "R", "R_mis",
    "BasicAttack_mis", "Idle", "Spell1", "Spell2", "Spell3", "Spell4",
    "Q_orb", "E_charm", "R_dash",
]

stems = ["{c}_Base_{a}", "{c}_{a}", "{a}", "Base_{a}"]

paths = set()
for d, s, a in itertools.product(dirs, stems, anims):
    name = s.format(c=CHAMP, a=a)
    for ext in (".preload", ".troy", ".dds", ".tex"):
        paths.add(f"{d}/{name}{ext}")

out = sys.argv[1]
with open(out, "w", encoding="utf-8") as handle:
    handle.write("\n".join(sorted(paths)))
print(f"wrote {len(paths)} candidates to {out}")
