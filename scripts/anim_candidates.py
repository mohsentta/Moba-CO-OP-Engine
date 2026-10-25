"""Generate candidate animation paths for a champion and write them one per line."""
import itertools
import sys

CHAMP = "Ahri"

dirs = [
    f"ASSETS/Characters/{CHAMP}/Animations",
    f"ASSETS/Characters/{CHAMP}/Animations/Base",
    f"ASSETS/Characters/{CHAMP}/Animations/Skin0",
    f"ASSETS/Characters/{CHAMP}/Animations/Skin00",
    f"ASSETS/Characters/{CHAMP}/Skins/Base",
    f"ASSETS/Characters/{CHAMP}/Skins/Base/Animations",
    f"ASSETS/Characters/{CHAMP}",
    f"DATA/Characters/{CHAMP}/Animations",
    f"DATA/Characters/{CHAMP}/Animations/Base",
]

anims = [
    "Idle1", "Idle2", "Idle3", "Idle", "Run", "Walk", "Attack1", "Attack2",
    "Attack3", "Crit", "Spell1", "Spell2", "Spell3", "Spell4", "Death",
    "Recall", "Dance", "Taunt", "Joke", "Laugh", "Respawn", "Channel",
    "BasicAttack", "Emote", "Idle1_Base",
]

stems = ["{c}_Base_{a}", "{c}_{a}", "{a}", "{c}_Base_{a}_skin0", "Base_{a}"]

paths = set()
for d, s, a in itertools.product(dirs, stems, anims):
    name = s.format(c=CHAMP, a=a)
    paths.add(f"{d}/{name}.anm")
    paths.add(f"{d}/{name.lower()}.anm")

# Control entries that are known to exist, to prove the search works.
paths.add(f"ASSETS/Characters/{CHAMP}/Skins/Base/{CHAMP}_Base.skn")

out = sys.argv[1]
with open(out, "w", encoding="utf-8") as handle:
    handle.write("\n".join(sorted(paths)))
print(f"wrote {len(paths)} candidates to {out}")
