# FieldHub v2 manufacturing release — 2026-10-10 revision 6

- **Release date:** 2026-10-10
- **Manufacturing status:** **Printed — five-board prototype batch**
- **Compared with:** `hardware/FieldHub_v2/revisions/2026-10-08-r5`
- **Release directory:** `hardware/FieldHub_v2/revisions/2026-10-10-r6`

This directory records the exact revision 6 source and manufacturing pack used
for the printed five-board prototype batch.

## Release decision

The pack was reviewed as suitable for a five-board prototype run, subject to
the normal JLCPCB assembly preview and DFM checks. The BOM, pick-and-place,
EasyEDA source, Gerbers, board outline, drill data, and the intended component
substitutions were cross-checked before the pack was archived.

Key checks:

- the BOM and pick-and-place files contain the same 264 unique designators;
- F2 uses `C474299` as two separate 4.7 kOhm pull-ups for `SDA1` and `SCL1`;
- R29-R32 use `C11702` and connect `A0-A3` to `A0_IN-A3_IN`, respectively;
- U52 uses `C70285` (`SN74LVC1G74DCUR`) with the intended DCU/VSSOP-8 pinout;
- the board outline, bottom copper, and drill geometry are unchanged from
  revision 5;
- all four 3.4 mm NPTH mounting holes remain present; and
- both compressed archives passed integrity checks.

The replacement U52 land pattern is compatible with the DCU package but is not
geometrically identical to the previous EasyEDA footprint. This difference was
reviewed and accepted for the prototype batch.

## Assembly handling

The test-point references, SJ4/SJ5 solder jumpers, and U1 custom through holes
do not have LCSC part numbers. They are PCB features or manual-fit positions and
must remain unplaced in the PCBA component assignment.

## Archived files

| File | SHA-256 |
|---|---|
| `HUB_BOM_Board1_PCB1_2026-10-10_v2_#6.csv` | `9454f1738c43dfc5abc172f9c1f383a8f0a96158eca1653bba1595e29666a0d3` |
| `HUB_PickAndPlace_PCB1_2026_10_10_v2__6.csv` | `2a25d47fb803dbdacf6de5ec9760e789dbb0e2144e93e6910f9fdcf9279be361` |
| `HUB_Gerber_PCB1_2026-10-10_v2_#6.zip` | `8139e3cf18dde4643b14ae04e5f5590ab83b4470fd8ce9b545de42048b33c29d` |
| `HUB_ProPrj_MotherShip_v2_2026-10-10_v2_#6.epro2` | `2405ca4124481170195c439466975b48cd493afec3ed2e5a56544a54cdf2e9c8` |

## Review limitation

This archive review does not replace EasyEDA ERC/DRC, the fabricator's DFM
checks, or physical bring-up and validation of the assembled boards.
