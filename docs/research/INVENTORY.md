# Function inventory and what "rebuild every function" means

Source: `src/*.c` (the Ghidra output) and `modules.tsv`, `renames.tsv`.

## Numbers

| metric | value |
|---|---|
| decompiled functions | **122,572** (`modules.tsv` rows) |
| decompiled source files | 1,196 (chunks of ≤1,200 functions) |
| decompiled C | 216,962,911 bytes (~217 MB) |
| original binary | 43,944,384 bytes (~44 MB) |
| functions with an inferred name | **26** (`renames.tsv`, non-fallback) |
| functions still named `sub_<rva>` | 122,546 (99.98%) |
| functions with a partially known signature | all (the C text has one), but with `undefined4`/`longlong` placeholders |

The 1196 files are not modules in the architectural sense; they are just
consecutive 1,200-function chunks (many files hit exactly 1,200).

## What is in those 122k functions

- **C/C++ runtime and third-party libraries**: zstd (`0xf05a00`), std::regex,
  std::string/std::vector/std::map, the MSVC CRT, exception machinery, TLS,
  security cookies, SIMD helpers. Reimplementing these is pointless — they are
  better replaced by crates or the platform. A large fraction of the 122k
  entries are these.
- **The game engine**: allocator, containers, RTTI-ish type registry, object
  system, scene graph, renderer, animation, particles, audio, input, UI.
- **Game logic**: champions, abilities, buffs, items, minions, AI, networking.
  These are thousands of functions and change every patch.

## Why a literal 1:1 rebuild is not a session-sized task

1. **Volume.** At an optimistic 30 minutes per function to read the decompiled
   body, recover the structs behind `longlong`/`undefined4`, write Rust, and
   test it, 122,572 functions is ~61,000 hours (~30 person-years). Even 5
   minutes each is ~10,000 hours.
2. **The decompile is lossy.** There are no type definitions, fields are byte
   offsets, globals overlap, inlined functions are duplicated, vtables are raw
   pointers, and control flow is `goto`-heavy. Each function is reverse
   engineering, not translation.
3. **Only 26 functions are named.** For the other 122,546 there is no
   statement of intent, so "did I port it correctly?" cannot be answered
   without building the whole call graph and testing behaviour.
4. **Non-code dependencies.** Riot's assets (WAD/BIN/SKN/ANM/TEX), shaders,
   encryption/signing, anti-cheat and networking are not in these functions.
5. **It cannot be verified.** Behavioural parity requires running the original
   next to the port; the original is a live, anti-cheat-protected game.

So "rebuild every function exactly" is, in practice, a clean-room
reimplementation of League of Legends — a multi-year team effort, not a
deliverable that can be produced here.

## The only workable method

Port by **subsystem and dependency order**, using the decompile as the
reference, replacing library code with crates, and testing each layer:

0. **Inventory + call graph** (this file, `modules.tsv`, per-module indexes).
1. **Foundations** — allocator (`Mem_Alloc` 0xf33c30), string
   (`String_Init` 0xf6aa10, `String_CopyFrom` 0xf6aa40, `String_Assign`
   0xf6d000), vector/map, math, RNG, TLS.
2. **Type registry** — `Registry_AppendField` 0xf6dbb0,
   `TypeRegistry_DefineType` 0xf6c070, the 0x30-byte field records.
3. **Object system** — `Registry_FindObjectById` 0x531740, object vtables,
   netId map.
4. **Replay path** — already ported: container, blocks, timeline, dispatch.
5. **Packet handlers** — the per-type payload decoders (this is what would
   unlock positions/animation state in replays).
6. **Renderer / audio / UI** — engine subsystems plus assets.

Milestones 0-4 are tractable and partly done. Milestone 5 is the bounded but
large reverse-engineering job that a replay viewer actually needs. Milestone 6
is the largest and least replay-specific.
