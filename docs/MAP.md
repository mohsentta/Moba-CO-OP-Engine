# Map archive and jungle pets

## Map11.wad.client (2.4 GB)

The map archive is where the interesting non-champion content lives: jungle
pets, terrain and map props. It does **not** parse with the same assumptions as
champion archives, and two details had to be fixed in the WAD reader:

1. **The TOC starts at 0x110**, not 0x10.
2. **The TOC contains empty slots** (hash set, sizes and offsets zero). Treating
   those as the end of the table truncated parsing at 18,954 of 28,463 entries —
   which silently hid everything past that point, including all pets.
3. Compression codes beyond the catalogue (up to 0x40) are legal; the reader
   must not stop on them.
4. The true entry count is derived from the smallest non-zero payload offset,
   since the first payload begins exactly where the TOC ends. A structural walk
   alone overruns into data (the bytes at the end of the map TOC are literally a
   filename).

With those fixed: `Ahri.wad` still reports 6,078 chunks (no regression) and
`Map11.wad` reports 28,461.

## Jungle pets

The jungle companions ("Durian" system) are ordinary character assets:

```
assets/characters/durian_<name>/skins/base/durian_<name>.skn   (mesh)
assets/characters/durian_<name>/skins/base/durian_<name>.skl   (skeleton)
assets/characters/durian_<name>/skins/base/durian_<name>.tex   (colour)
```

31 of them: `miner`, `duckbill`, `pengu`, `grumpylion`, `balldragon`, `choncc`,
`creepycat`, `elegantdragon`, … Note the naming differs from champions: no
`_Base` suffix, and the colour map is `<name>.tex` rather than `_TX_CM.tex`.
No `.anm` files are listed for them, so they currently render in bind pose.

`lolview_sandbox --pet durian_miner` loads one (verified: 1,554 vertices,
rendered beside the champion).

## Terrain: `.mapgeo`

```
data/maps/mapgeometry/map11/base_srx.mapgeo   23,050,092 -> 91,726,588 bytes
```

Header begins:

```
4f 45 47 4d  "OEGM"
12 00 00 00  version 18
02 00 00 00
00 00 00 00
15 00 00 00  count 21
...          string table: "BAKED_DIFFUSE_TEXTURE", ...
```

So terrain is a single ~92 MB mesh with material/bucket records referencing
baked diffuse textures. The format is **not yet parsed**; doing so is the
remaining work for real terrain. Until then the sandbox ground is procedural.

## Map ground texture

`assets/maps/info/map11/2dlevelminimap.dds` is listed in the community hash
dictionary but does **not** resolve in `Map11.wad.client` for this build, so it
cannot currently be used as a flat Rift ground.
