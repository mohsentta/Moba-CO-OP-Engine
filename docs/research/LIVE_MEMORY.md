
## Live-process findings (running game, replay loaded)

Started `League of Legends.exe "EUW1-8020814588.rofl"` (PID 16828) and read it with
ReadProcessMemory.

- `sub_c99740` is a **factory**, not a map: a static jump table at RVA
  `0xcaa134`, 1266 entries of u32 RVAs, message id -> constructor. Dumped to
  `port/packet_ids.tsv`.
- Message 37 (`0x25`): constructor `0xc9a458` -> real ctor `0xc34d40`, which
  writes `id = 0x25` and installs vtable `0x1855e60`. Slot 2 returns 6 (body
  size); slot 6 (`0xcea620`) is a writer that packs flags plus a 7-bit varint
  with per-byte obfuscation (`^0x2a`, `-2`, rotate). Message 37 carries a small
  id/enum plus one varint, not positions.
- `g_ReplayObjectList` (RVA `0x1c88fb0`) points to a vector object:
  `+0 vtable`, `+8 begin`, `+0x10 u32 count`, `+0x14 u32 capacity`. In the
  running replay it holds exactly 10 objects, one per player.
- Object layout confirmed live:
  `+0x20 netId`, `+0x158 team`, `+0x15c x (float)`, `+0x160 y (float)`,
  `+0x164 z (float)`. Example live values: net 0x10401 team 1
  (2291.0, 52.8, 12448.0); net 0x10428 team 2 (2857.3, 49.5, 12561.8).
  y stays near 50 (height); x/z are map coordinates.
- Exact-value needle search (`needle`) for live coordinates across payloads
  produced only scattered hits at unrelated times - positions are not stored
  as plain absolute floats or absolute u24/1000 in the payloads.
- Synthetic space/P do not pause the client (positions kept moving), so the
  ten-position set match (`match10`) could not be run against a frozen frame.
