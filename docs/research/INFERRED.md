# Inferred roles around the known replay path

Seeded from the functions identified in the decompile. Distance 1 meansndirect caller/callee; distance 2 is context only.

| d | rva | function | likely role | evidence |
|---|---|---|---|---|
| 0 | `0x275330` | `sub_275330` | per-object score map lookup/insert | called by 0x69ac90 (Replay_RunEventsUntilTime: dispatch events up to the clock) |
| 0 | `0x69ac90` | `Replay_RunEventsUntilTime` | Replay_RunEventsUntilTime: dispatch events up to the clock | KNOWN: Replay_RunEventsUntilTime: dispatch events up to the clock |
| 0 | `0x6a16a0` | `sub_6a16a0` | vector append for chunk records | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 0 | `0x6a4eb0` | `sub_6a4eb0` | chunk record range map lookup | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 0 | `0x6a8420` | `sub_6a8420` | per-object replay accumulator reset | called by 0x6be560 (replay controller tick: countdown, per-object state, event walk) |
| 0 | `0x6a8c70` | `sub_6a8c70` | score accumulation for status flags | called by 0x6a8fd0 (per-object replay scoring/vision pass) |
| 0 | `0x6a8fd0` | `sub_6a8fd0` | per-object replay scoring/vision pass | called by 0x6be560 (replay controller tick: countdown, per-object state, event walk) |
| 0 | `0x6a92f0` | `Replay_ProcessEventsLoop` | Replay_ProcessEventsLoop: peek-loop variant with float/netId fields | KNOWN: Replay_ProcessEventsLoop: peek-loop variant with float/netId fields |
| 0 | `0x6a98f0` | `Replay_ProcessEventsLoop2` | Replay_ProcessEventsLoop2: three-cursor event walk | KNOWN: Replay_ProcessEventsLoop2: three-cursor event walk |
| 0 | `0x6ab1a0` | `sub_6ab1a0` | vector grow for chunk records | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 0 | `0x6abca0` | `Replay_PeekEvent` | Replay_PeekEvent: read record at cursor, bump payload refcount | KNOWN: Replay_PeekEvent: read record at cursor, bump payload refcount |
| 0 | `0x6af080` | `Replay_ParseBlock` | Replay_ParseBlock: marker-bit block parser (time, size, id, param) | KNOWN: Replay_ParseBlock: marker-bit block parser (time, size, id, param) |
| 0 | `0x6b1580` | `sub_6b1580` | replay gate | called by 0x9aa080 (replay-mode Update: sync, gate, update A/B, camera follow) |
| 0 | `0x6b1b60` | `sub_6b1b60` | replay file loader: reads RIOT header and calls Replay_LoadChunks | KNOWN: replay file loader: reads RIOT header and calls Replay_LoadChunks |
| 0 | `0x6b2890` | `Replay_LoadChunks` | Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors | KNOWN: Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors |
| 0 | `0x6b2e90` | `Replay_BuildRecord` | Replay_BuildRecord: iterates blocks into record objects | KNOWN: Replay_BuildRecord: iterates blocks into record objects |
| 0 | `0x6b6040` | `Replay_DecompressChunk` | Replay_DecompressChunk: stored copy or ZSTD_decompress | KNOWN: Replay_DecompressChunk: stored copy or ZSTD_decompress |
| 0 | `0x6b8260` | `sub_6b8260` | replay update path A | calls 0x6be560 (replay controller tick: countdown, per-object state, event walk); caller itself: replay update path A |
| 0 | `0x6b8460` | `sub_6b8460` | replay sync: last_sync = clock | called by 0x9aa080 (replay-mode Update: sync, gate, update A/B, camera follow) |
| 0 | `0x6ba930` | `Replay_SeekCursor` | Replay_SeekCursor: set/clamp cursor, cache timestamp | KNOWN: Replay_SeekCursor: set/clamp cursor, cache timestamp |
| 0 | `0x6bb2d0` | `Replay_SeekTime` | Replay_SeekTime: binary search over 0x18-byte event array | KNOWN: Replay_SeekTime: binary search over 0x18-byte event array |
| 0 | `0x6be560` | `sub_6be560` | replay controller tick: countdown, per-object state, event walk | KNOWN: replay controller tick: countdown, per-object state, event walk |
| 0 | `0x6bf600` | `sub_6bf600` | replay update path B | calls 0x6be560 (replay controller tick: countdown, per-object state, event walk); caller itself: replay update path B |
| 0 | `0x9aa080` | `sub_9aa080` | replay-mode Update: sync, gate, update A/B, camera follow | KNOWN: replay-mode Update: sync, gate, update A/B, camera follow |
| 1 | `0x2035f0` | `sub_2035f0` | CONTEXT string/object construct helper | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x2036b0` | `sub_2036b0` | CONTEXT std::string assign/copy (SSO buffer) | called by 0x69ac90 (Replay_RunEventsUntilTime: dispatch events up to the clock) |
| 1 | `0x23c030` | `sub_23c030` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x23cbc0` | `sub_23cbc0` | CONTEXT payload buffer grow/copy (Mem_Alloc + memcpy) | called by 0x6af080 (Replay_ParseBlock: marker-bit block parser (time, size, id, param)) |
| 1 | `0x23e870` | `sub_23e870` | CONTEXT vector resize helper | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x2474b0` | `sub_2474b0` | CONTEXT CRT fatal: range-check failure | called by 0x6be560 (replay controller tick: countdown, per-object state, event walk) |
| 1 | `0x247e00` | `sub_247e00` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x2494c0` | `sub_2494c0` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x261fd0` | `sub_261fd0` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x272310` | `sub_272310` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x288710` | `sub_288710` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x28b180` | `sub_28b180` | adjacent to the replay path (role unknown) | called by 0x6a8fd0 (per-object replay scoring/vision pass) |
| 1 | `0x28e3f0` | `sub_28e3f0` | adjacent to the replay path (role unknown) | called by 0x6a8fd0 (per-object replay scoring/vision pass) |
| 1 | `0x2b6630` | `sub_2b6630` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x2b66a0` | `sub_2b66a0` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x2b68f0` | `sub_2b68f0` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x2b69a8` | `sub_2b69a8` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x2dfbc0` | `sub_2dfbc0` | CONTEXT object type predicate A (type id compare) | called by 0x6a92f0 (Replay_ProcessEventsLoop: peek-loop variant with float/netId fields) |
| 1 | `0x2dfc40` | `sub_2dfc40` | CONTEXT object type predicate B (type id compare) | called by 0x6a92f0 (Replay_ProcessEventsLoop: peek-loop variant with float/netId fields) |
| 1 | `0x3bee00` | `sub_3bee00` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x3d4050` | `sub_3d4050` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x426420` | `sub_426420` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x4f1d60` | `sub_4f1d60` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x6a1e00` | `sub_6a1e00` | adjacent to the replay path (role unknown) | calls 0x6b1580 (replay active gate: flags, timer, speed checks); caller itself: unknown |
| 1 | `0x6a5180` | `sub_6a5180` | event dispatch helper (called from the event walk) | called by 0x6a92f0 (Replay_ProcessEventsLoop: peek-loop variant with float/netId fields) |
| 1 | `0x6a5eb0` | `sub_6a5eb0` | adjacent to the replay path (role unknown) | calls 0x6b1580 (replay active gate: flags, timer, speed checks); caller itself: unknown |
| 1 | `0x6a8c7b` | `sub_6a8c7b` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x6a8c95` | `sub_6a8c95` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x6a8f00` | `sub_6a8f00` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x6a8f6c` | `sub_6a8f6c` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x6a8fe8` | `sub_6a8fe8` | adjacent to the replay path (role unknown) | calls 0x6a8c70 (score accumulation for status flags); caller itself: unknown |
| 1 | `0x6a8fff` | `sub_6a8fff` | adjacent to the replay path (role unknown) | calls 0x6a8c70 (score accumulation for status flags); caller itself: unknown |
| 1 | `0x6a9040` | `sub_6a9040` | adjacent to the replay path (role unknown) | calls 0x6a8c70 (score accumulation for status flags); caller itself: unknown |
| 1 | `0x6a90a9` | `sub_6a90a9` | adjacent to the replay path (role unknown) | calls 0x6a8c70 (score accumulation for status flags); caller itself: unknown |
| 1 | `0x6a90be` | `sub_6a90be` | adjacent to the replay path (role unknown) | calls 0x6a8c70 (score accumulation for status flags); caller itself: unknown |
| 1 | `0x6a91dc` | `sub_6a91dc` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x6a9650` | `sub_6a9650` | adjacent to the replay path (role unknown) | called by 0x6a8fd0 (per-object replay scoring/vision pass) |
| 1 | `0x6a9903` | `sub_6a9903` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x6a995a` | `sub_6a995a` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x6a9ad0` | `sub_6a9ad0` | adjacent to the replay path (role unknown) | called by 0x6a8fd0 (per-object replay scoring/vision pass) |
| 1 | `0x6aa8b0` | `sub_6aa8b0` | adjacent to the replay path (role unknown) | calls 0x6b1580 (replay active gate: flags, timer, speed checks); caller itself: unknown |
| 1 | `0x6ac2c0` | `sub_6ac2c0` | adjacent to the replay path (role unknown) | called by 0x6b1580 (replay active gate: flags, timer, speed checks) |
| 1 | `0x6b293f` | `sub_6b293f` | adjacent to the replay path (role unknown) | calls 0x6a4eb0 (chunk record range map lookup); caller itself: unknown |
| 1 | `0x6b29aa` | `sub_6b29aa` | adjacent to the replay path (role unknown) | calls 0x6a4eb0 (chunk record range map lookup); caller itself: unknown |
| 1 | `0x6b29b0` | `sub_6b29b0` | adjacent to the replay path (role unknown) | calls 0x6a4eb0 (chunk record range map lookup); caller itself: unknown |
| 1 | `0x6b34a0` | `sub_6b34a0` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x6b829a` | `sub_6b829a` | driver/owner of the replay event walk | calls 0x6be560 (replay controller tick: countdown, per-object state, event walk); caller itself: unknown |
| 1 | `0x6bab00` | `sub_6bab00` | adjacent to the replay path (role unknown) | calls 0x6b1580 (replay active gate: flags, timer, speed checks); caller itself: unknown |
| 1 | `0x6baec0` | `sub_6baec0` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x6bafc0` | `sub_6bafc0` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x6bbb90` | `sub_6bbb90` | adjacent to the replay path (role unknown) | calls 0x6b1580 (replay active gate: flags, timer, speed checks); caller itself: unknown |
| 1 | `0x6bc750` | `sub_6bc750` | code that launches replay loading | calls 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks); caller itself: unknown |
| 1 | `0x6bc756` | `sub_6bc756` | code that launches replay loading | calls 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks); caller itself: unknown |
| 1 | `0x6bc75e` | `sub_6bc75e` | code that launches replay loading | calls 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks); caller itself: unknown |
| 1 | `0x6bd9d0` | `sub_6bd9d0` | adjacent to the replay path (role unknown) | calls 0x6a4eb0 (chunk record range map lookup); caller itself: unknown |
| 1 | `0x6bd9fb` | `sub_6bd9fb` | adjacent to the replay path (role unknown) | calls 0x6a4eb0 (chunk record range map lookup); caller itself: unknown |
| 1 | `0x6bda0a` | `sub_6bda0a` | adjacent to the replay path (role unknown) | calls 0x6a4eb0 (chunk record range map lookup); caller itself: unknown |
| 1 | `0x731f30` | `sub_731f30` | adjacent to the replay path (role unknown) | called by 0x6a8420 (per-object replay accumulator reset) |
| 1 | `0x9a6610` | `sub_9a6610` | helper inside the replay update path | called by 0x6b8260 (replay update path A) |
| 1 | `0x9aab60` | `sub_9aab60` | adjacent to the replay path (role unknown) | calls 0x6b8460 (replay sync: last_sync = clock); caller itself: unknown |
| 1 | `0x9c09c0` | `sub_9c09c0` | adjacent to the replay path (role unknown) | calls 0x6bf600 (replay update path B); caller itself: unknown |
| 1 | `0x9c09d1` | `sub_9c09d1` | adjacent to the replay path (role unknown) | calls 0x6bf600 (replay update path B); caller itself: unknown |
| 1 | `0x9c09f2` | `sub_9c09f2` | adjacent to the replay path (role unknown) | calls 0x6bf600 (replay update path B); caller itself: unknown |
| 1 | `0x9c0aef` | `sub_9c0aef` | adjacent to the replay path (role unknown) | calls 0x6bf600 (replay update path B); caller itself: unknown |
| 1 | `0x9c5310` | `sub_9c5310` | adjacent to the replay path (role unknown) | calls 0x6bf600 (replay update path B); caller itself: unknown |
| 1 | `0x9c6460` | `sub_9c6460` | adjacent to the replay path (role unknown) | calls 0x275330 (per-object score map lookup/insert); caller itself: unknown |
| 1 | `0x9c6630` | `sub_9c6630` | adjacent to the replay path (role unknown) | calls 0x6b1580 (replay active gate: flags, timer, speed checks); caller itself: unknown |
| 1 | `0xa1a580` | `sub_a1a580` | adjacent to the replay path (role unknown) | calls 0x6b1580 (replay active gate: flags, timer, speed checks); caller itself: unknown |
| 1 | `0xf0f510` | `sub_f0f510` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0xf21800` | `sub_f21800` | event dispatch helper (called from the event walk) | called by 0x69ac90 (Replay_RunEventsUntilTime: dispatch events up to the clock) |
| 1 | `0xf336d0` | `sub_f336d0` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0xf390d0` | `sub_f390d0` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0xf3fed0` | `sub_f3fed0` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0xf4f0a0` | `sub_f4f0a0` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0xf6ace0` | `sub_f6ace0` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0xf6f470` | `sub_f6f470` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0xf71830` | `sub_f71830` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0xf71b00` | `sub_f71b00` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0xf74290` | `sub_f74290` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0xf7d4a0` | `sub_f7d4a0` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 1 | `0x101ae40` | `sub_101ae40` | payload reader called by Replay_ParseBlock | called by 0x6af080 (Replay_ParseBlock: marker-bit block parser (time, size, id, param)) |
| 1 | `0x15b0840` | `sub_15b0840` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 1 | `0x15b0c20` | `sub_15b0c20` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 1 | `0x15b1110` | `sub_15b1110` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 1 | `0x15b23e0` | `sub_15b23e0` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 1 | `0x15b3b60` | `sub_15b3b60` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 1 | `0x15b40e0` | `sub_15b40e0` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x15b44e0` | `sub_15b44e0` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x15b5980` | `sub_15b5980` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 1 | `0x15b5c20` | `sub_15b5c20` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 1 | `0x15b5c60` | `sub_15b5c60` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 1 | `0x15b61a0` | `sub_15b61a0` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 1 | `0x15b62e0` | `sub_15b62e0` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 1 | `0x15b6540` | `sub_15b6540` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x15b6cb0` | `sub_15b6cb0` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x15b7170` | `sub_15b7170` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x15b7730` | `sub_15b7730` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x15b78e0` | `sub_15b78e0` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b2890 (Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors) |
| 1 | `0x17a5750` | `sub_17a5750` | chunk-chain helper called by Replay_LoadChunks | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x17cebdc` | `sub_17cebdc` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x17db2c0` | `sub_17db2c0` | event dispatch helper (called from the event walk) | called by 0x69ac90 (Replay_RunEventsUntilTime: dispatch events up to the clock) |
| 1 | `0x1809fa0` | `sub_1809fa0` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x180a0a0` | `sub_180a0a0` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |
| 1 | `0x180a740` | `sub_180a740` | replay header/file helper called by the loader | called by 0x6b1b60 (replay file loader: reads RIOT header and calls Replay_LoadChunks) |

Total functions in the 2-hop replay neighbourhood: 128
