"""Wide sweep for particle/VFX asset paths across several naming conventions."""
import itertools
import sys

CHAMP = "Ahri"

dirs = [
    f"ASSETS/Characters/{CHAMP}/Skins/Base/Particles",
    f"ASSETS/Characters/{CHAMP}/Skins/Base/Particles/Textures",
    f"ASSETS/Characters/{CHAMP}/Skins/Base/VFX",
    f"ASSETS/Characters/{CHAMP}/Skins/Base/Spells",
    f"ASSETS/Characters/{CHAMP}/Particles",
    f"ASSETS/Characters/{CHAMP}/Spells",
    f"ASSETS/Characters/{CHAMP}/VFX",
    f"ASSETS/Spells/{CHAMP}",
    f"ASSETS/Spells/Characters/{CHAMP}",
    f"ASSETS/VFX/Characters/{CHAMP}",
    f"ASSETS/VFX/{CHAMP}",
]

tokens = [
    "Q", "W", "E", "R", "Q_mis", "W_mis", "E_mis", "R_mis", "QMissile", "WMissile",
    "E_missile", "Q_hit", "W_hit", "E_hit", "R_hit", "Q_cast", "W_cast", "E_cast",
    "R_cast", "Passive", "BasicAttack", "Idle", "Orb", "Charm", "FoxFire", "Dash",
    "Q_tar", "W_tar", "E_tar", "R_tar",
]

prefixes = [f"{CHAMP}_Base", CHAMP, "Base", ""]

exts = [".preload", ".troy", ".dds", ".tex"]

paths = set()
for d, pre, token, ext in itertools.product(dirs, prefixes, tokens, exts):
    stem = f"{pre}_{token}" if pre else token
    paths.add(f"{d}/{stem}{ext}")
    paths.add(f"{d}/{stem.lower()}{ext}")

out = sys.argv[1]
with open(out, "w", encoding="utf-8") as handle:
    handle.write("\n".join(sorted(paths)))
print(f"wrote {len(paths)} candidates to {out}")
