# Legal posture

This project is designed to be defensible while doing something inherently
grey: reading another company's game data.

**It is not legal advice.** If you intend to publish or distribute anything,
talk to a lawyer.

## The rules the code enforces

1. **No game content is distributed.** The repository contains no models,
   textures, archives, executables, decompiled code or extracted data. The
   `.gitignore` refuses the relevant extensions, and CI must never fetch them.
2. **The user supplies the assets.** `AssetSource` has no default constructor
   and no bundled fallback. Running the program without `--game-dir` pointing at
   a real installation is a hard error, not a degraded mode.
3. **Read-only.** Nothing under the installation is written, patched or
   modified. The game must be closed; the program never touches a running
   client, its process, its memory, or its anti-cheat.
4. **No gameplay advantage.** There is no skin changer, unlock, script, memory
   reader, packet manipulator or automation here. Those are what Riot pursues.
5. **No DRM circumvention.** Plain files on disk are parsed as they are; no
   decryption keys, no bypasses.
6. **Neutral naming and clear disclaimer.** The project does not use Riot marks
   as its own name, and every entry point prints the disclaimer.
7. **Free and non-commercial.** No sales, no crowdfunding, no ads.
8. **Your code, your license.** Format readers are written from observation and
   documented in `docs/research/`. No GPL third-party parser is vendored, so the
   project stays MIT.

## Where the assets legitimately come from

* **Data Dragon** (Riot's official CDN) is the sanctioned source for icons,
  splash art and audio, and may be used in fan projects under Riot's fan-content
  policy.
* **Meshes, skeletons, animations and effects** are not on Data Dragon; they
  exist only inside the local installation. That is why this tool reads the
  user's own copy instead of downloading anything.

## Things to avoid

* Committing screenshots that embed extracted textures, or "sample" model files
  as test fixtures. Tests use synthetic triangles and synthetic archives.
* Publishing a hosted build that serves Riot assets from your server. Host the
  code; let the user's machine supply the data.
* Reimplementing gameplay precisely from decompiled code — that is a derivative
  work of Riot's binary, and a much sharper legal problem than reading models.
  The sandbox uses our own simplified logic.
