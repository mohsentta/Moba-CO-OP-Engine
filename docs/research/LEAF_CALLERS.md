# Functions that call leaf functions

Role is inferred from the mix of leaf behaviours the function calls.

| leaf calls | rva | function | inferred role | leaf profile |
|---|---|---|---|---|
| 80 | `0xcdf960` | `sub_cdf960` | bit manipulation / codec | algox6 bittrickx64 tablex10 |
| 36 | `0x106efb0` | `sub_106efb0` | small-operator algorithm | algox1 opx35 |
| 21 | `0xd0ae80` | `sub_d0ae80` | mixed bittrick+table | bittrickx14 init_globalsx2 tablex5 |
| 19 | `0x7f0140` | `sub_7f0140` | small-operator algorithm | algox1 opx18 |
| 18 | `0x7431c0` | `sub_7431c0` | small-operator algorithm | opx18 |
| 18 | `0xce1f70` | `sub_ce1f70` | bit manipulation / codec | bittrickx13 tablex5 |
| 18 | `0xe51c10` | `sub_e51c10` | mixed bittrick+algo | algox3 bittrickx10 init_globalsx2 tablex3 |
| 18 | `0x10705b0` | `sub_10705b0` | small-operator algorithm | algox1 opx17 |
| 14 | `0xce3080` | `sub_ce3080` | bit manipulation / codec | bittrickx12 tablex2 |
| 14 | `0xe6ab60` | `sub_e6ab60` | mixed bittrick+init_globals | algox1 bittrickx9 init_globalsx2 tablex2 |
| 13 | `0x5e5c90` | `sub_5e5c90` | mixed table+algo | algox2 eqx1 init_globalsx2 mulx2 simdx2 tablex4 |
| 12 | `0x5e5f13` | `sub_5e5f13` | mixed table+algo | algox2 eqx1 init_globalsx2 mulx1 simdx2 tablex4 |
| 12 | `0xcdf320` | `sub_cdf320` | bit manipulation / codec | bittrickx9 tablex3 |
| 12 | `0xe022f0` | `sub_e022f0` | bit manipulation / codec | bittrickx9 init_globalsx2 tablex1 |
| 11 | `0x2112f0` | `sub_2112f0` | small-operator algorithm | algox1 opx10 |
| 11 | `0x392d80` | `sub_392d80` | small-operator algorithm | opx11 |
| 11 | `0xaa35a0` | `sub_aa35a0` | small-operator algorithm | opx11 |
| 11 | `0xcc1350` | `sub_cc1350` | bit manipulation / codec | algox1 bittrickx8 simdx1 tablex1 |
| 11 | `0x1070130` | `sub_1070130` | small-operator algorithm | algox1 opx10 |
| 10 | `0x394430` | `sub_394430` | small-operator algorithm | opx10 |
| 10 | `0x9d3ae0` | `sub_9d3ae0` | small-operator algorithm | opx10 |
| 10 | `0xcae0f0` | `sub_cae0f0` | mixed bittrick+table | bittrickx6 simdx1 tablex3 |
| 10 | `0xdfea60` | `sub_dfea60` | bit manipulation / codec | bittrickx7 init_globalsx2 tablex1 |
| 9 | `0x5e7790` | `sub_5e7790` | table-driven lookup | simdx1 tablex8 |
| 9 | `0xa5c5d0` | `sub_a5c5d0` | small-operator algorithm | opx9 |
| 9 | `0xaa3180` | `sub_aa3180` | small-operator algorithm | opx9 |
| 9 | `0xd290f0` | `sub_d290f0` | bit manipulation / codec | bittrickx7 tablex2 |
| 9 | `0xea4cf0` | `sub_ea4cf0` | bit manipulation / codec | bittrickx8 tablex1 |
| 9 | `0x1308e70` | `sub_1308e70` | mixed simd+bittrick | algox1 bittrickx2 init_globalsx1 simdx4 tablex1 |
| 8 | `0x523a40` | `sub_523a40` | small-operator algorithm | opx8 |
| 8 | `0x566d40` | `sub_566d40` | small-operator algorithm | opx8 |
| 8 | `0x60bf60` | `sub_60bf60` | small-operator algorithm | opx8 |
| 8 | `0x829b50` | `sub_829b50` | small-operator algorithm | opx8 |
| 8 | `0x8b76d0` | `sub_8b76d0` | small-operator algorithm | opx8 |
| 8 | `0x8b8a60` | `sub_8b8a60` | small-operator algorithm | opx8 |
| 8 | `0xecb9b0` | `sub_ecb9b0` | bit manipulation / codec | algox1 bittrickx6 tablex1 |
| 8 | `0x1070d60` | `sub_1070d60` | small-operator algorithm | opx8 |
| 8 | `0x1071150` | `sub_1071150` | small-operator algorithm | algox1 opx7 |
| 8 | `0x14b1580` | `sub_14b1580` | SIMD kernel | algox1 fmathx1 simdx6 |
| 8 | `0x1561a40` | `sub_1561a40` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x1561bfd` | `sub_1561bfd` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x1562510` | `sub_1562510` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x1562607` | `sub_1562607` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x15626ec` | `sub_15626ec` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x1563730` | `sub_1563730` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x15638bb` | `sub_15638bb` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x1563f90` | `sub_1563f90` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x156409a` | `sub_156409a` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x15641fd` | `sub_15641fd` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x1564b90` | `sub_1564b90` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x1564c94` | `sub_1564c94` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x1564dae` | `sub_1564dae` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x15656f0` | `sub_15656f0` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x15657f8` | `sub_15657f8` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 8 | `0x1565942` | `sub_1565942` | mixed simd+algo | algox2 fmathx1 simdx5 |
| 7 | `0x1cba40` | `sub_1cba40` | small-operator algorithm | algox1 opx6 |
| 7 | `0x1e7a70` | `sub_1e7a70` | small-operator algorithm | opx7 |
| 7 | `0x43da10` | `sub_43da10` | small-operator algorithm | opx6 tablex1 |
| 7 | `0x4d4c20` | `sub_4d4c20` | small-operator algorithm | opx7 |
| 7 | `0x6b1b60` | `sub_6b1b60` | mixed simd+add | addx1 bittrickx1 notx1 set_d8x1 simdx2 tablex1 |
| 7 | `0x9d3da0` | `sub_9d3da0` | small-operator algorithm | opx7 |
| 7 | `0xaa4830` | `sub_aa4830` | small-operator algorithm | opx7 |
| 7 | `0xb1a0d0` | `sub_b1a0d0` | small-operator algorithm | opx7 |
| 7 | `0xd795e0` | `sub_d795e0` | bit manipulation / codec | algox1 bittrickx5 tablex1 |
| 7 | `0xe07fa0` | `sub_e07fa0` | bit manipulation / codec | bittrickx5 init_globalsx2 |
| 7 | `0xe9e150` | `sub_e9e150` | bit manipulation / codec | algox1 bittrickx6 |
| 7 | `0xeb4c60` | `sub_eb4c60` | mixed bittrick+table | algox1 bittrickx4 tablex2 |
| 7 | `0x1189370` | `sub_1189370` | small-operator algorithm | opx7 |
| 7 | `0x12b56a0` | `sub_12b56a0` | generic algorithm | algox5 simdx1 tablex1 |
| 7 | `0x12d68d0` | `sub_12d68d0` | mixed table+algo | algox1 init_globalsx1 modx1 tablex4 |
| 7 | `0x12d694a` | `sub_12d694a` | mixed table+algo | algox1 init_globalsx1 modx1 tablex4 |
| 7 | `0x13bb320` | `sub_13bb320` | mixed algo+bittrick | algox4 bittrickx2 simdx1 |
| 7 | `0x13bd140` | `sub_13bd140` | mixed algo+bittrick | algox4 bittrickx2 simdx1 |
| 6 | `0xe3c70` | `sub_0e3c70` | small-operator algorithm | opx6 |
| 6 | `0x144aa0` | `sub_144aa0` | small-operator algorithm | opx6 |
| 6 | `0x15bb30` | `sub_15bb30` | small-operator algorithm | opx6 |
| 6 | `0x1b9cc0` | `sub_1b9cc0` | small-operator algorithm | opx6 |
| 6 | `0x1d76a0` | `sub_1d76a0` | small-operator algorithm | opx6 |
| 6 | `0x2fbcc0` | `sub_2fbcc0` | small-operator algorithm | opx5 tablex1 |
| 6 | `0x392510` | `sub_392510` | small-operator algorithm | opx5 tablex1 |
| 6 | `0x392b70` | `sub_392b70` | small-operator algorithm | opx6 |
| 6 | `0x393ee0` | `sub_393ee0` | small-operator algorithm | opx5 tablex1 |
| 6 | `0x524490` | `sub_524490` | small-operator algorithm | opx6 |
| 6 | `0x597760` | `sub_597760` | mixed simd+algo | algox2 simdx3 tablex1 |
| 6 | `0x5bf2e0` | `sub_5bf2e0` | small-operator algorithm | opx6 |
| 6 | `0x744460` | `sub_744460` | small-operator algorithm | opx6 |
| 6 | `0x7ef230` | `sub_7ef230` | small-operator algorithm | opx6 |
| 6 | `0x7f09d0` | `sub_7f09d0` | small-operator algorithm | opx6 |
| 6 | `0x8b8010` | `sub_8b8010` | small-operator algorithm | opx6 |
| 6 | `0x964d30` | `sub_964d30` | small-operator algorithm | opx6 |
| 6 | `0xaa1f20` | `sub_aa1f20` | small-operator algorithm | opx6 |
| 6 | `0xb918d0` | `sub_b918d0` | small-operator algorithm | opx6 |
| 6 | `0xd1c6f0` | `sub_d1c6f0` | bit manipulation / codec | bittrickx5 tablex1 |
| 6 | `0xd1e8e0` | `sub_d1e8e0` | mixed bittrick+table | bittrickx4 tablex2 |
| 6 | `0xd26dd0` | `sub_d26dd0` | bit manipulation / codec | bittrickx6 |
| 6 | `0xd8a0b0` | `sub_d8a0b0` | bit manipulation / codec | bittrickx6 |
| 6 | `0xdf6d30` | `sub_df6d30` | mixed bittrick+init_globals | bittrickx4 init_globalsx2 |
| 6 | `0xe6ffa0` | `sub_e6ffa0` | bit manipulation / codec | bittrickx6 |
| 6 | `0xe9b620` | `sub_e9b620` | bit manipulation / codec | bittrickx5 tablex1 |
| 6 | `0xefbf50` | `sub_efbf50` | mixed algo+init_globals | algox2 init_globalsx2 simdx2 |
| 6 | `0xefc23f` | `sub_efc23f` | mixed algo+init_globals | algox2 init_globalsx2 simdx2 |
| 6 | `0xf13aa0` | `sub_f13aa0` | mixed table+add | addx1 algox1 notx1 simdx1 tablex2 |
| 6 | `0xf13abc` | `sub_f13abc` | mixed table+add | addx1 algox1 notx1 simdx1 tablex2 |
| 6 | `0xf13b29` | `sub_f13b29` | mixed table+add | addx1 algox1 notx1 simdx1 tablex2 |
| 6 | `0x106feb0` | `sub_106feb0` | small-operator algorithm | algox1 opx5 |
| 6 | `0x12b3d10` | `sub_12b3d10` | mixed table+algo | algox1 init_globalsx1 tablex4 |
| 6 | `0x12b3d2b` | `sub_12b3d2b` | mixed table+algo | algox1 init_globalsx1 tablex4 |
| 6 | `0x12b98b0` | `sub_12b98b0` | mixed table+algo | algox1 init_globalsx1 tablex4 |
| 6 | `0x12e8930` | `sub_12e8930` | mixed algo+simd | algox4 simdx1 tablex1 |
| 6 | `0x14aa460` | `sub_14aa460` | mixed table+algo | algox1 fmathx1 simdx1 tablex3 |
| 6 | `0x152d880` | `sub_152d880` | SIMD kernel | algox1 simdx5 |
| 6 | `0x1625300` | `sub_1625300` | mixed algo+simd | algox3 simdx3 |
| 6 | `0x16d2ec0` | `sub_16d2ec0` | SIMD kernel | simdx6 |
| 6 | `0x175b4c0` | `sub_175b4c0` | SIMD kernel | algox1 simdx5 |
| 6 | `0x175b4ec` | `sub_175b4ec` | SIMD kernel | algox1 simdx5 |
| 6 | `0x175f1e0` | `sub_175f1e0` | SIMD kernel | simdx5 tablex1 |
| 5 | `0xc9b60` | `sub_0c9b60` | small-operator algorithm | opx5 |
| 5 | `0xe2dd0` | `sub_0e2dd0` | small-operator algorithm | opx5 |
| 5 | `0xfc5f0` | `sub_0fc5f0` | small-operator algorithm | opx5 |
| 5 | `0x113f90` | `sub_113f90` | small-operator algorithm | opx5 |

Total callers of leaves: 24141
