# Latest hw.dll Metadata Reverse Notes

Last updated: 2026-07-11

Goal: rebuild local metadata from the latest `hw.dll` parser behavior rather
than relying on captured official payloads. Official pcap payloads are still
encrypted unless a keylog with the target Client Random is available.

## Wire Format

Latest metadata packet id is `91`.

Payload:

```text
uint8  metadata_id
uint8  chunk_flag
uint16 chunk_size
bytes  chunk_payload
```

For one-shot ZIP metadata, `chunk_flag = 0x05` and `chunk_payload` starts with
`PK 03 04`.

## Handler Table

Confirmed latest table entries:

| ID | File | Handler |
| -: | ---- | ------- |
| 0 | `resource/MapList.csv` | `FUN_02670f30` |
| 1 | `resource/itemBox.csv` | `FUN_02670e60` |
| 2 | `resource/MapModeV2/ModeList.csv` | `LAB_02670ea0` |
| 12 | `Matching.csv` | `FUN_02670eb0` |
| 20 | `weaponparts.csv` | `FUN_02670f80` |
| 21 | `MileageShop.csv` | `FUN_02671000` |
| 27 | `resource/GameModeList.csv` | `FUN_02671080` |
| 30 | `resource/zombiez/progress_unlock.csv` | `FUN_02671170` |
| 31 | `ReinforceMaxLv.csv` | `FUN_026711f0` |
| 32 | `ReinforceMaxEXP.csv` | `FUN_02671270` |
| 35 | `resource/item.csv` | `FUN_026712f0` |
| 39 | `HonorShop.csv` | `FUN_02671610` |
| 40 | `resource/ItemExpireTime.csv` | `FUN_02671690` |
| 41 | `resource/scenariotx/scenariotx_common.json` | `FUN_02671750` |
| 42 | `resource/scenariotx/scenariotx_dedi.json` | `FUN_02671770` |
| 43 | `resource/scenariotx/shopitemlist_dedi.json` | `FUN_02671790` |
| 48 | `ppsystem/config.json` | `FUN_026718d0` |
| 51 | `resource/zombiecompetitive/ZBCompetitive.json` | `FUN_02671930` |
| 53 | `resource/ModeEvent/ModeEvent.csv` | `FUN_02671950` |
| 54 | `resource/CPShop/EventShop.csv` | `FUN_02671970` |
| 55 | `resource/ClanWar/FamilyTotalWarMap.csv` | `FUN_026719f0` |
| 56 | `resource/ClanWar/FamilyTotalWar.json` | `FUN_02671aa0` |

## Parser Findings

### Common ZIP/CSV Wrapper

`FUN_0277bf70` receives the ZIP payload, extracts/parses it into a string table
object, and returns true when the table buffer and dimensions are available.
Wrapper handlers then call a manager-specific parser with that table.

### ID 39 HonorShop.csv

Handler:

```text
FUN_02671610
  FUN_020392c0()        -> CHonorShopMgr singleton
  FUN_020394f0(table)   -> parse HonorShop rows
```

`FUN_020394f0` starts at row index `1`, so row `0` is treated as a header.
It expects at least 25 columns. Observed column access:

| Column | Expected meaning from local header |
| -----: | ---------------------------------- |
| 0 | ProductId |
| 1 | RN |
| 2 | price |
| 3 | begintime, parsed as date string unless `"0"` |
| 4 | endtime, parsed as date string unless `"0"` |
| 5 | Totalcount |
| 6 | PersonalCount |
| 7 | PersonalDailyCount |
| 8 | saleRate |
| 9 | SaleBeginTime, parsed as date string unless `"0"` |
| 10 | SaleEndtime, parsed as date string unless `"0"` |
| 11 | tag |
| 12 | minHonorScore |
| 14 | fixed numeric field |
| 15 | fixed numeric field |
| 16-21 | six fixed numeric fields packed into bit/count fields |
| 22-23 | two fixed numeric fields |
| 24 | extra relation count |
| 25+ | `column 24` extra numeric relation values |

Date parser format:

```text
%d-%d-%d %d:%d
```

So non-zero date fields must be strings like `2026-07-11 04:00`.

Current local `HonorMoneyShop.csv` passes the parser-access validator,
including the dynamic `25 + column24` read range. If this metadata still
crashes, the next suspect is manager-side semantic validation or dependency
state after construction, not a simple missing-column parse fault.

### ID 30 resource/zombiez/progress_unlock.csv

Handler:

```text
FUN_02671170
  SharedDataMgr::vftable + 0x510 -> FUN_025ce030(table)
```

Local shape validated so far:

| Column | Type |
| -----: | ---- |
| 0 | integer item id |
| 1 | float required progress |
| 2 | integer notify flag |
| 3 | integer random weapon flag |

The latest parser requires exactly four columns before it performs the row
parse. Local empty `randomweapon` values were normalized to `0`.

### ID 35 resource/item.csv

Handler:

```text
FUN_026712f0
  FUN_02585970() -> item manager singleton
  vtable + 0x04  -> parses item.csv table
```

The wrapper then refreshes inventory, weapon, encyclopedia, and class-related
managers. Because multiple dependent managers are triggered, item metadata must
be parsed before many dependent metadata tables.

### ID 40 resource/ItemExpireTime.csv

Handler:

```text
FUN_02671690
  vtable + 0x230 creates generic table named "resource/ItemExpireTime"
  table.vtable + 0x0c loads the ZIP payload
  item manager receives the table through vtable + 0x24 target
```

This is a generic named table path. The file name inside ZIP should remain
`resource/ItemExpireTime.csv`.

### ID 41 resource/scenariotx/scenariotx_common.json

Handler:

```text
FUN_02671750
  FUN_0208c0b0() -> ScenarioTXMgr singleton
  ScenarioTXMgr.vtable + 0x0c(0, payload)
```

`FUN_02079c30` constructs `ScenarioTXMgr`.

### ID 30/31/32

Handlers:

```text
30 FUN_02671170 -> SharedDataMgr::vftable + 0x510 -> FUN_025ce030
31 FUN_026711f0 -> SharedDataMgr::vftable + 0x694 -> FUN_025d11e0
32 FUN_02671270 -> SharedDataMgr::vftable + 0x698 -> FUN_025d0f50
```

`DAT_03f8993c` is initialized in `FUN_0260bbc0` with the object built by
`FUN_0259a3a0`; its vtable is `SharedDataMgr::vftable` at `0x02c5c76c`.

ReinforceMaxLv (`FUN_025d11e0`) reads columns `0..7` as integers and, when the
table has 9 columns, parses column `8` as an optional metadata/list string.

ReinforceMaxEXP (`FUN_025d0f50`) reads column `0` as the key and columns `1..9`
as float/numeric values. The old local file had only 8 total columns, so two
trailing `0` columns were added.

These three metadata files now pass the parser-access validator and are enabled
server-side again. `HonorShop.csv` remains disabled because its parser-access
shape is valid, so any remaining crash is likely semantic or dependency-state
related rather than a simple missing-column fault.

### ID 2 resource/MapModeV2/ModeList.csv

The current handler at `0x02670ea0` is exactly `xor al, al; ret`. The local
payload has been rebuilt with the correct latest ZIP entry path, but it must
not be sent while this handler remains a rejecting stub.

### ID 21 MileageShop.csv

`FUN_02671000` dispatches to `FUN_0203ef60`. The parser starts after one header
row, reads 27 columns, treats columns `3,4,9,10` as optional date strings, and
uses numeric values in columns `0..2`, `5..8`, and `11..26`. The local file
passes this concrete access/type schema.

### ID 54 resource/CPShop/EventShop.csv

`FUN_02671970` dispatches to `FUN_0203ddc0`. The parser skips two header rows.
It reads 25 columns, accepts date strings in columns `7,8,9`, and numeric
values in columns `0..6` and `10..24`. The local file passes this concrete
access/type schema.

## Current Practical Plan

1. Keep sending metadata in latest table order.
2. Load ZIP payload sources from `bin/MetadataArtifacts` by the latest
   internal metadata path, falling back to legacy `bin/Data` only when an
   artifact is absent.
3. Keep `resource/MapModeV2/ModeList.csv` disabled for automatic transmission
   while its current handler remains `xor al, al; ret`.
4. Send oversized ZIP payloads with chunked metadata packets. The current
   artifact `resource/item.csv` compresses to about 70 KB with the server ZIP
   library, so it is sent as multiple chunks instead of being skipped.

## Full Local Rebuild

`tools/rebuild_latest_metadata.py` now rebuilds all 22 confirmed latest
handler payloads as deterministic ZIP files under `bin/RebuiltMetadata`.
It enforces the latest ZIP entry names, validates CSV row shape and JSONC
syntax, applies the known concrete parser schemas, and rejects payloads that
exceed the one-shot `uint16` chunk limit.

The legacy embedded metadata id `2` ZIP was replaced with `bin/Data/ModeList.csv`.
The server now creates its ZIP with the latest internal path
`resource/MapModeV2/ModeList.csv`. The current latest handler body is only
`xor al, al; ret`, so id `2` remains rebuildable but is not enabled for
automatic transmission.

`ItemExpireTime.csv` contained date-divider rows such as `;231101`. These were
not item records and could reach the latest generic table parser as one-column
rows, so they were removed. Every remaining non-empty row has at least the
item id and expiry value expected by the table path.

Latest full rebuild result: `rebuilt=22 errors=0`. The largest one-shot ZIP is
id `35 resource/item.csv` at about 56.8 KB, below the 65,535-byte chunk limit.

## Latest `hw_2026.dll` artifact reconstruction

`tools/assemble_metadata_artifacts.py` no longer emits placeholder comments or
`_placeholder` JSON objects. The remaining server-only paths were classified by
their concrete handlers in `hw_2026.dll`:

| Path | Concrete parser/handler | Minimal representation |
| --- | --- | --- |
| `models/SkinWeaponInfo_server.json` | `0x0203e630` | empty JSON table `{}`; data loop starts at row 1 |
| `SeasonBadgeShop.csv` | `0x026718d0` | header-only table; dispatched through badge manager |
| `resource/buff/classmastery.json` | `0x0203f880` | empty JSON table `{}`; data loop starts at row 1 |
| `resource/WeaponAscend.csv` | `0x01ff1080` | empty JSON object; this nominal CSV path uses the JSON parser |
| `resource/zombi/PerkParam.csv` | `0x025d0a80` | 7-column header; parser reads columns 0..6 from row 1 onward |
| `AnniversaryShop.csv` | `0x025d0090` | 8-column header; parser reads columns 0..7 from row 1 onward |
| `ZCoinShop.csv` | `0x025d0280` | 4-column header; optional synthesis triples begin at column 4 |

The handlers at `0x02671150`, `0x02671160`, and `0x02671cb0` through
`0x02671ce0` are exactly `xor al, al; ret`. Therefore `badwordadd.csv`,
`badworddel.csv`, the three Anniversary JSON paths, and `SynthesisItem.csv`
have no payload that can be accepted by this client build. Their artifact files
are intentionally zero bytes and are labelled `unsupported_by_hw` in the
manifest instead of containing fabricated metadata.

The assembled tree contains all 39 requested paths plus `manifest.json` under
`bin/MetadataArtifacts`. Current result: 22 server sources, 4 extracted current
client originals, 7 parser-derived minimal files, and 6 disabled-handler files.

`tools/validate_metadata_artifacts.py` validates this assembled tree directly
against the latest registered path table. Current result: `validated=39`,
`errors=0`, `chunked=1`. The chunked payload is `resource/item.csv`: raw size
588,142 bytes and deterministic ZIP size about 71 KB.
