# Asset hw.dll Startup and Server Response Flow

## Scope

This document describes the startup path of:

- `D:\project\CSONLINE\asset\Bin\hw.dll`
- Ghidra program: `/NewFolder/asset_hw.dll`
- x86 image base: `0x01D00000`

The primary goal is to identify the server responses that this client expects
from authentication through lobby bootstrap. Engine rendering and platform
initialization are included only where they control network state.

## Main Result

The current server metadata IDs are not aligned with this `hw.dll`.

`Packet_Metadata` allocates 67 records of `0x2C` bytes. A handler stored at
offset `O` belongs to metadata ID:

```text
id = (O - 0x28) / 0x2C
```

For example:

```text
Matching.csv:             (0x1B4 - 0x28) / 0x2C = 9
weaponparts.csv:          (0x314 - 0x28) / 0x2C = 17
resource/GameModeList.csv:(0x448 - 0x28) / 0x2C = 24
```

The server currently uses 12, 20, and 27 for those entries. Those values select
different records or empty records in the client. This is a direct candidate
for metadata parse failure, disconnect, or a later crash caused by missing
initialized data.

The packet files in `Packets_sampel/2` independently confirm the Ghidra-derived
IDs: the captured startup stream sends Matching as 9, weaponparts as 17, and
GameModeList as 24.

## Engine Entry Path

Windows loader activity is not the game startup entry:

1. `entry` at `0x028DA694` initializes the security cookie and dispatches CRT
   initialization.
2. User `DllMain` at `0x028DAC89` only calls `DisableThreadLibraryCalls` on
   process attach.
3. The launcher calls export `F` at `0x024BD750`.
4. `F` requests `VENGINE_LAUNCHER_API_VERSION002` and returns its interface.
5. The interface run thunk at `0x024BBAB0` enters `0x024BBF80`.

`0x024BBF80` parses the command line, initializes the filesystem, BlackCipher,
video, the engine, and the Windows message/frame loop. It invokes the engine
load method at `0x024BDF00`, which enters `Sys_Init` at `0x024BCCC0`.

`Sys_Init` loads `VFileSystem009`, allocates the engine heaps, runs `Host_Init`
at `0x0243B410`, loads `GameUI007`, and initializes BaseUI. Network packet
objects are registered during the game/client initialization reached from this
path.

## Authentication Entry

There are two credential paths:

- Login UI command `Login`: `0x02651940`
- Passport/launcher authentication: `0x0232A150` -> `0x02518F10`

Both paths converge on the auth manager and then `0x025193A0`, which iterates
the supplied game-server endpoints and attempts a connection. On success the
client records the authenticated state and advances the engine state to 5.

This means a successful Nexon/launcher credential result alone is insufficient.
The game-server connection and the response sequence below must also complete.

## Confirmed Initial Server Responses

`Packets_sampel/2` begins with the following server-to-client sequence. The
payload definitions below were verified against the loaded asset `hw.dll`.

| Order | Packet ID | Meaning | Required payload |
| ---: | ---: | --- | --- |
| 0 | 0 | Version | `uint8 result/versionState` |
| 1 | 1 | Reply | `uint8 result`, string message, `uint8 stringCount`, strings |
| 2 | 17 | SessionID | `uint32 sessionId` |
| 3 | 150 | UserStart | identity and region block described below |
| 4 | 7 | Crypt type 0 | direction, method, key, IV |
| 5 | 7 | Crypt type 1 | direction, method, key, IV |
| 6+ | 91 | Metadata | startup metadata stream |

### Version, ID 0

The parser at `0x026A54F0` consumes one byte. The sample is:

```text
00
```

### Reply, ID 1

The parser at `0x02688C60` consumes:

```text
uint8 result
string message
uint8 extraStringCount
string extraStrings[extraStringCount]
```

The successful sample is:

```text
00 "S_REPLY_OK\0" 00
```

The message is not a replacement for the result byte. Both fields are parsed.

### SessionID, ID 17

The parser at `0x0213BDB0` consumes exactly one `uint32` and stores it in the
packet object.

### UserStart, ID 150

The parser at `0x026A36B0` consumes:

```text
uint32 userId
string accountId
string characterName
uint8 firstConnect
uint8 countryCode
uint8 regionCode
uint32 accountOrSessionValue
uint8 unknownA
uint8 unknownB
```

The function writes account/character/first-connect/country/region values into
the crash reporter and global client state. If `firstConnect != 0`, it also
initializes the first-connect UI/state path. Omitting this packet leaves state
that later lobby and user packets expect unset.

### Crypt, ID 7

The parser at `0x026632A0` first reads:

```text
uint8 direction      // 0 or 1
uint8 method         // sample uses 2
```

For method 2 under `TLSv1`, it then reads:

```text
byte key[32]
byte iv[32]
```

Each sample packet is therefore 67 bytes including the outer packet ID. The
type 0 and type 1 samples use the same key/IV and differ in the direction byte.
The parser calls `CRYPT_ERROR` if cipher setup fails.

## Metadata Wire Format

The normal file-backed parser at `0x0267AE20` expects:

```text
uint8 metadataId
uint8 chunkFlags
uint16 chunkSize
byte data[chunkSize]
```

Confirmed flag bits:

- `0x01`: begin
- `0x02`: middle/continuation
- `0x04`: final and parse
- `0x05`: one-shot begin + final

Values outside `1..7`, or zero, are rejected as `Received wrong flag`.
On final, the accumulated bytes are opened as an archive and passed to the
handler registered for the metadata ID.

## Correct File-Backed Metadata IDs

The table below comes directly from the `Packet_Metadata` constructor at
`0x02676CD0`.

| ID | Handler offset | Expected archive path |
| ---: | ---: | --- |
| 0 | `0x028` | `resource/MapList.csv` |
| 1 | `0x054` | `resource/itemBox.csv` |
| 2 | `0x080` | `resource/MapModeV2/ModeList.csv` |
| 9 | `0x1B4` | `Matching.csv` |
| 17 | `0x314` | `weaponparts.csv` |
| 18 | `0x340` | `MileageShop.csv` |
| 24 | `0x448` | `resource/GameModeList.csv` |
| 25 | `0x474` | `badwordadd.csv` |
| 26 | `0x4A0` | `badworddel.csv` |
| 27 | `0x4CC` | `resource/zombiez/progress_unlock.csv` |
| 28 | `0x4F8` | `ReinforceMaxLv.csv` |
| 29 | `0x524` | `ReinforceMaxEXP.csv` |
| 32 | `0x5A8` | `resource/item.csv` |
| 33 | `0x5D4` | `voxel/voxel_list.csv` |
| 34 | `0x600` | `voxel/voxel_item.csv` |
| 35 | `0x62C` | `resource/codis/codisdata.cso` |
| 36 | `0x658` | `HonorShop.csv` |
| 37 | `0x684` | `resource/ItemExpireTime.csv` |
| 38 | `0x6B0` | `resource/scenariotx/scenariotx_common.json` |
| 39 | `0x6DC` | `resource/scenariotx/scenariotx_dedi.json` |
| 40 | `0x708` | `resource/scenariotx/shopitemlist_dedi.json` |
| 41 | `0x734` | `EpicPieceShop.csv` |
| 42 | `0x760` | `models/SkinWeaponInfo_server.json` |
| 44 | `0x7B8` | `SeasonBadgeShop.csv` |
| 45 | `0x7E4` | `ppsystem/config.json` |
| 46 | `0x810` | `resource/buff/classmastery.json` |
| 48 | `0x868` | `resource/zombiecompetitive/ZBCompetitive.json` |
| 50 | `0x8C0` | `resource/ModeEvent/ModeEvent.csv` |
| 51 | `0x8EC` | `resource/CPShop/EventShop.csv` |
| 52 | `0x918` | `resource/ClanWar/FamilyTotalWarMap.csv` |
| 53 | `0x944` | `resource/ClanWar/FamilyTotalWar.json` |
| 56 | `0x9C8` | `resource/WeaponAscend.csv` |
| 58 | `0xA20` | `resource/zombi/PerkParam.csv` |
| 59 | `0xA4C` | `AnniversaryShop.csv` |
| 60 | `0xA78` | `resource/Anniversary/GlobalAnniversary.json` |
| 61 | `0xAA4` | `resource/Anniversary/AnniversaryLottery.json` |
| 62 | `0xAD0` | `resource/Anniversary/AnniversaryTicket.json` |
| 63 | `0xAFC` | `resource/Synthesis/SynthesisItem.csv` |
| 66 | `0xB80` | `ZCoinShop.csv` |

IDs without a file handler can still be valid. `0x02677B30` has direct
structured-data branches for IDs such as 3, 4, 5, 6, 8, 15, 16, 19, 20, 21,
30, 31, 43, 49, 54, 55, 57, 64, and 65. They must not be sent using the ZIP
format merely because the outer packet ID is 91.

## Captured Initial Metadata Order

The sample's first file-backed startup burst is:

```text
0  MapList
9  Matching
1  ClientTable
17 weaponparts
24 GameModeList
27 progress_unlock
28 ReinforceMaxLv
29 ReinforceMaxEXP
31 structured metadata
255 hash records (four packets)
37 ItemExpireTime
36 HonorShop
38 scenariotx_common
39 scenariotx_dedi
40 shopitemlist_dedi
42 SkinWeaponInfo/WeaponProp data
45 ppsystem
48 ZBCompetitive
50 ModeEvent
53 FamilyTotalWar
52 FamilyTotalWarMap
56 WeaponAscend
58 PerkParam
63 Synthesis
```

It then sends structured metadata IDs 3, 43, chunked 4, 30, 6, 64, 65, 8,
49, 54, 55, and 57. Later in the same session, item data is sent as ID 32
using begin/final chunks, followed by voxel list ID 33 and codis data ID 35.

The important distinction is that the initial stream is not simply every ZIP
handler in ascending order. Some datasets are structured packets and some
large/file-backed datasets are deliberately delayed.

## Transition After Metadata

In the sample, the initial metadata burst is followed by:

```text
Event(159)
UMsg(67) burst
additional metadata (20, 15)
inventory/account/lobby bootstrap packets
Lobby(153)
GameMatch(99)
late item/voxel/codis metadata (32, 33, 35)
UserStartStep(123) near the end of the captured sequence
```

Therefore metadata success alone does not enter the lobby. The server must
continue with a state-appropriate user bootstrap. However, these later packet
layouts must be taken from the current `hw.dll` parsers; only their order and
presence are established by the sample at this stage.

## Server Changes Implied by This Analysis

1. Correct `EMetadataPacketType` to the IDs calculated from the current
   constructor before testing any metadata payload again.
2. Keep ZIP metadata and direct structured metadata as separate send paths.
3. Restore the confirmed initial response order: Version, Reply, SessionID,
   UserStart, Crypt 0, Crypt 1, then metadata.
4. Do not interpret a parser failure under the old metadata IDs as malformed
   CSV/JSON. The wrong handler was selected before the archive was parsed.
5. After the corrected initial metadata burst parses successfully, map the
   post-metadata user/bootstrap packet layouts from their current vtables,
   beginning with Event, UMsg, Lobby, and GameMatch.

### Concrete mismatch in the current server

The current authentication senders for Version, Reply, SessionID, UserStart,
and Crypt match the parser layouts above. The remaining blockers are in the
state transition and metadata selection:

- `OnCryptPacket` currently sends `Transfer(2)` (or diagnostic
  `HostServer(81/1)`) immediately after the crypt acknowledgement. The sample
  has no server transfer packet at this point, and runtime analysis already
  showed that this login socket does not have an ID 2 handler. The server
  should wait for the client's subsequent `Login(3)` packet instead.
- `OnLoginPacket` already has the correct default shape after that point: send
  metadata on the same login socket.
- The default minimal metadata set currently means MapList 0, Matching 12, and
  GameModeList 27. For this DLL it must mean 0, 9, and 24.

The current minimal stream therefore behaves as follows:

```text
0  -> MapList handler                  (correct)
12 -> no Matching file handler         (ignored/wrong state)
27 -> progress_unlock handler receives GameModeList archive (wrong parser)
```

This explains why reducing the stream to three packets did not make startup
safe: two of the three packet IDs were still wrong, and the last packet selected
an incompatible, valid handler rather than simply being ignored.

## Confidence and Limits

High confidence:

- Engine interface entry and version.
- Version, Reply, SessionID, UserStart, Crypt payload layouts.
- Metadata chunk framing.
- All file-backed metadata IDs and paths in the table.
- Agreement between the Ghidra table and `Packets_sampel/2` startup IDs.

Not yet fully established:

- Whether every later packet in `Packets_sampel/2` has an unchanged payload
  layout for this exact asset build.
- The complete current layouts of the post-metadata Event/UMsg/inventory/lobby
  bootstrap packets.
- Which of the later metadata datasets are mandatory before the first visible
  lobby frame, as opposed to being optional feature initialization.

## Runtime Validation (2026-07-16)

The corrected authentication sequence was exercised with the asset client and
the local `localuser` account. The client accepted Version, Reply, SessionID,
UserStart, and both Crypt packets, logged `Game Server Login Success`, and then
sent the encrypted Login(3) packet. This confirms that authentication and the
client-to-server encrypted direction reach the intended state.

The server then sent metadata on the same socket with the corrected IDs. Both
the minimal set and the configured file-backed set still ended after about ten
seconds. The client-side metadata parse hook produced no metadata entry before
shutdown. Replaying packets 6 through 149 from `Packets_sampel/2` in one burst
also ended, which shows that a blind server-packet replay cannot replace the
request/state transitions between those packets.

Additional confirmed differences from the captured initial stream were found:

- `ClientTable` ID 1 was omitted by `SendMetadata`; it is now restored after
  Matching ID 9 and before WeaponParts ID 17.
- Diagnostic three-file metadata is no longer the default. It is only enabled
  by `CSNZ_ASSET_MIN_METADATA=1`.
- Item ID 32 and Codis ID 35 are no longer sent in the initial metadata burst;
  the capture sends them in a late phase after account/lobby bootstrap.
- WeaponAuction ID 8 and Mileage ID 18 are likewise not file-backed members of
  the captured initial burst.

The next protocol boundary is therefore not user authentication. It is the
server-to-client encrypted metadata stream and its completion transition. The
next reverse-engineering pass must verify the outbound cipher state after
Crypt(1), then map the structured metadata sequence beginning at IDs 31, 255,
3, 43, 4, and 30 before implementing the Event/UMsg/lobby continuation.
