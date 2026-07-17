# CSNZ_Server Progress

Last updated: 2026-07-13

## Current Goal

Adapt the server login/bootstrap protocol to the latest CSNZ `hw.dll` so the launcher can authenticate locally and proceed to the lobby without crashing or disconnecting.

The injected dump DLL / internal packet-capture launcher path has been abandoned.
Further work should focus on making `CSNZ_Server` itself match the client protocol.

## Metadata Source Rule

`bin/LiveMetadata` is the authoritative metadata source for local client tests.
These files were captured from the live server and must not be regenerated from
`Data` or `MetadataArtifacts` during protocol work. Fallback metadata remains
available for older diagnostics, but the default latest-client stream should only
send payloads that are backed by `LiveMetadata` or explicitly isolated with
`CSNZ_METADATA_FILTER`.

## Implemented

- Server-focused continuation on 2026-07-13:
    - Rebuilt the Release server target successfully:
      `bin/Release/CSNZ_Server.exe`.
    - Started the rebuilt server from `bin`; it loads `bin/LiveMetadata` and
      listens on TCP 30002 with SSL enabled.
    - Removed the default server-side metadata skips for live ZIP payloads such
      as `Item.csv.zip`, `CodisData.csv.zip`, `MileageShop`, and `EventShop`.
    - Enabled default dedicated-host user bootstrap after delayed metadata.
      Set `CSNZ_DEDI_USER_BOOTSTRAP=0` only when intentionally testing the
      metadata-only host path.
    - Adjusted latest metadata order so `FamilyTotalWarMap(55)` is sent before
      `FamilyTotalWar(56)`.
    - Kept unsafe legacy-only layouts suppressed by default where the latest
      layout is not verified or collides with current metadata IDs.
- Updated local login path so the latest client can authenticate with `localuser/localpass1`.
- Added `Transfer(2)` sender based on the latest `hw.dll` `Packet_Transfer` parser:
    - `uint32 ip` in network byte order on the wire.
    - `uint16 port` in host order; the client converts it with `htons()` before connecting.
    - `uint8 transfer kind`.
    - string ticket/account token.
- `OnCryptPacket()` now sends `Transfer(2)` by default after crypt acknowledgement.
    - The previous `HostServer(81/1)` transfer remains available only for diagnostics with `CSNZ_HOSTSERVER_TRANSFER=1`.
- Added newer login packet parsing diagnostics.
- Updated supported build marker to the latest local test build.
- Disabled or deferred several legacy post-login packets that the latest login socket cannot parse at the current stage.
    - Legacy `UserStartStep(123)`, `UserStart(150)`, `UserUpdateInfo(157)` are known to be unsafe before the latest socket transitions to the correct handler table.
    - Inventory/loadout startup packets were made no-op in the latest login path to avoid known legacy-layout crashes.
- Added detailed server packet logs:
    - RX/TX sequence, size, id, and hex preview.
    - TCP disconnect reason fields.

## Current Experimental State

`SendLatestLoginCrypt()` currently sends the latest login crypt bootstrap after `S_REPLY_OK`:

- type `0` crypt packet is sent in plaintext.
- server RC4 output is enabled after type `0`.
- type `1` crypt packet is sent encrypted while keeping the same socket packet sequence.
- the server then waits for the latest client crypt acknowledgement before sending `HostServer Transfer(81/1)`.

`CPacketManager::SendCrypt()` is currently set to:

- method `2`
- wire key length `32`
- wire iv length `32`

This was based on the latest `hw.dll` parser behavior for `TLSv1`.

The latest `hw.dll` method `2` cipher descriptor uses a 16-byte effective key length, so the server EVP RC4 context is set to 16 bytes while still sending the latest `TLSv1` 32-byte key/iv fields on the wire.

## Important Findings

- Login succeeds server-side:
    - User is added.
    - `S_REPLY_OK` is sent.
- Sending legacy post-login packets immediately after login is incorrect for the latest login socket.
- After `S_REPLY_OK(id=1)`, the latest launcher socket expands its handler table from `0/1/7/81` to include `109`.
- `Help(109)` with payload `00` is parsed successfully, but it does not yet cause the client to send the expected next packet.
- Crypt experiments:
    - `method=4`, 64-byte key/iv, no local stream cipher: client parses only type 0, then eventually closes.
    - `method=2`, 32-byte key/iv, server RC4 enabled before type 1: client parses type 0, then fails with `ReadPacket result=8`.
    - `method=2`, 32-byte key/iv, plus `Help(109)`: `Help(109)` and crypt type 0 parse, but type 1 is not acknowledged and the client later closes.
- Latest Ghidra finding:
    - `ReadPacket` is at Ghidra `0x02778BA0`.
    - return `8` is caused by server packet sequence mismatch.
    - Resetting the server sequence between crypt type `0` and type `1` is invalid for latest `hw.dll`.
- Latest runtime finding:
    - Server now sends crypt packets as `seq=3` and `seq=4`.
    - Client input crypt flags are enabled after type `0`.
    - Type `1` parses and the client sends plaintext acknowledgement packet `id=12`.
    - The server switches TLS sockets to raw `send/recv` once crypt input/output flags are enabled; keeping `wolfSSL_send` after crypt output is invalid for the latest client.
- The previous post-ACK bootstrap was wrong:
    - Packets `UserStartStep(123)`, `UserStart(150)`, `UserUpdateInfo(157)`, `Metadata(91)`, `GameMatch(99)`, and `Event(159)` are read by the latest client on the auth socket but have no registered parser there.
    - Ghidra confirms `id=81` is `HostServer`; subtype `1` transfers to an IP/port and opens a new connection.
    - Current experiment sends `HostServer Transfer(81/1)` to `127.0.0.1:<configured tcp port>` after crypt ACK.
- Latest correction:
    - `HostServer(81/1)` is not the normal lobby transfer path. It enters a host/dedicated-server style flow.
    - Ghidra confirms `Packet_Transfer(2)` has the normal server transfer parser and calls the follow-up routine that opens a new connection and builds `TransferLogin(5)`.
    - Runtime before the latest launcher-auth finalizer patch confirmed the server-sent `Transfer(2)` reaches the client, but the current login socket still has no `id=2` handler. This points to a launcher/auth state registration issue rather than a server packet layout issue.
- The transfer creates a second TLS connection.
    - The second connection sends `HostServer AddServer(81/0)` followed by `HostServer Unk3(81/3)`.
    - Sending metadata immediately from `CDedicatedServer` construction is too early; early metadata arrives before the latest client has registered its metadata handler.
    - Metadata is now delayed until about two seconds after `HostServer Unk3(81/3)` so the latest client has time to register the metadata handler.
    - `VoxelURLs(103)` is temporarily skipped for this host-server bootstrap because the latest test disconnected immediately after it was sent.
- Latest metadata packet format was corrected:
    - Latest `hw.dll` common metadata parser expects `metadataId`, then a one-byte chunk flag, then `uint16 size`, then payload.
    - The server previously sent `metadataId + uint16 size + payload`, so `MapList.csv` failed with `Received wrong flag`.
    - Zip/blob metadata now sends single-chunk flag `0x05` (`begin | final`) before the size.
- The host-server metadata sender now follows the latest `hw.dll` metadata table order for the currently mapped local payloads:
    - `0 MapList`
    - `2 ModeList` if enabled
    - `12 Matching`
    - `21 MileageShop`
    - `27 GameModeList`
    - `35 item`
    - `40 ItemExpireTime`
    - `41 scenariotx_common`
    - `42 scenariotx_dedi`
    - `43 shopitemlist_dedi`
    - `48 ppsystem`
    - `51 ZBCompetitive`
    - `53 ModeEvent`
    - `54 EventShop`
    - `55 FamilyTotalWarMap`
    - `56 FamilyTotalWar`
- Metadata entries `30 progress_unlock.csv`, `31 ReinforceMaxLv.csv`, and
  `32 ReinforceMaxEXP.csv` were remapped against the latest `hw.dll` parser and
  re-enabled in the active server config.
- Asset-based testing now uses `CSNZ_Server/bin/LiveMetadata` as the metadata
  source. `weaponparts.csv.zip` was restored to the live-server payload after a
  local rebuild experiment and is kept unchanged for parser diagnostics.
- The default host metadata stream skips fallback-only `ModeList`,
  `MileageShop`, and `EventShop`, plus `weaponparts` while its latest
  `hw.dll` parser path is still the first blocking point. Individual ids can be
  tested with `CSNZ_METADATA_FILTER`.
- The asset client currently runs in a minimal metadata mode by default:
  `MapList(0)`, `Matching(12)`, and `GameModeList(27)`.
  Set `CSNZ_FULL_METADATA=1` to send the broader `LiveMetadata` stream again.
  Set `CSNZ_METADATA_FILTER=<ids>` to isolate individual metadata handlers.
- Asset `hw.dll` parser/runtime blockers found so far:
    - `20 weaponparts.csv`: parser call did not return.
    - `31 ReinforceMaxLv.csv`: parser call intermittently did not return.
    - `32 ReinforceMaxEXP.csv`: parser call did not return.
    - `35 resource/item.csv`: chunked parser receives begin/middle chunks but
      returns `0`; final chunk was not observed in the client dump.
    - `38 resource/codis/codisdata.cso`: parser call did not return.
    - `39 HonorShop.csv`: parser call did not return.
- Metadata chunk size is now configurable with `CSNZ_METADATA_CHUNK_SIZE`.
  The asset default is `0x7000` (`28672`) because the 64000-byte live-sized
  chunk produced `ReadPacket result=7` before the metadata handler.
- Live server `C:\Nexon\Counter-Strike Online\Bin\Error.log` shows the startup
  metadata order parses as:
  `MapList`, `Matching`, `weaponparts`, `GameModeList`, `progress_unlock`,
  `ReinforceMaxLv`, `ReinforceMaxEXP`, `ItemExpireTime`, `HonorShop`,
  `scenariotx_common`, `scenariotx_dedi`, `shopitemlist_dedi`,
  `SkinWeaponInfo_server`, `ppsystem`, `ZBCompetitive`, `ModeEvent`,
  `FamilyTotalWar`, `FamilyTotalWarMap`, `WeaponAscend`, `PerkParam`,
  and `Synthesis`.
- The same live log first reports hash mismatches for `resource/item.csv` and
  `resource/codis/codisdata.cso`, then parses both after the lobby/web UI
  starts. The local server now follows that shape by excluding item/codis from
  the initial metadata burst and sending them after the dedicated host bootstrap
  when `CSNZ_DEDI_USER_BOOTSTRAP=1`.
- `EpicPieceShop.csv` and `MileageShop.csv` are live-delivered later on user/UI
  actions, not in the first startup burst, so they remain out of the default
  initial stream.
- Added a full latest-handler metadata rebuild pipeline covering all 22
  confirmed ids. It validates exact ZIP entry paths, CSV/JSONC structure,
  known parser schemas, and the one-shot `uint16` payload limit.
- Replaced the legacy embedded id `2` ZIP with a rebuilt ModeList payload whose
  internal path is `resource/MapModeV2/ModeList.csv`. The latest id `2`
  handler returns false immediately, so this payload remains disabled.
- Removed non-data date-divider rows from `ItemExpireTime.csv`; the full local
  rebuild now completes with `rebuilt=22 errors=0`.
- `39 HonorShop.csv` still remains disabled. Its parser-access shape is now
  known, but any remaining failure is likely semantic/dependency state rather
  than a simple missing-column crash.
- Server logs now include `TX latest metadata: id=..., name=...` before each metadata send.
- Official pcap `official_game_wireshark_222_122_48_21.pcapng` was parsed with `python-pcapng`.
    - Observed official flow: after TLS/login, server sends a dense server-to-client burst totaling about `368 KB`.
    - The capture is encrypted/custom-crypted after handshake, so packet sizes/timing are useful, but metadata IDs are not directly visible from the pcap alone.
- Added pcap/metadata recovery helpers:
    - `tools/pcap_metadata_report.py` summarizes the official TCP/TLS flow and post-login burst.
    - `tools/extract_metadata_from_stream.py` extracts metadata ZIP payloads from a decrypted CSNZ `U` packet stream.
    - Running the extractor directly on the pcap returns no valid metadata packets; this confirms the pcap itself is still encrypted and cannot be used as a direct metadata source.
- Checked `C:\Users\user\Desktop\sslkey.log` against the official pcap.
    - The target game flow is `192.168.0.12:57740 -> 222.122.48.21:8001`.
    - Its TLS Client Random is `fdd2cd7998e05043baea42cae173504f6cb63731d2568302b7abcae4fd11f8d4`.
    - The supplied keylog has many entries and matches three other HTTPS flows in the pcap, but it has `0` hits for the target game flow.
    - Therefore this keylog cannot decrypt the captured game server metadata stream.
- Current local enabled metadata ZIP payload total is still below the official
  server-to-client burst of roughly `368 KB`.
    - This implies the official server sends additional metadata and/or follow-up bootstrap data beyond the currently enabled local set.
    - Actual file restoration requires either TLS/application secrets for the capture or a live client-side dump after decryption.
- The latest client sends login packet id `3` with a layout different from the old server's original parser.

## Next Steps

1. Run the rebuilt server and launcher, then compare server `TX latest metadata` lines with launcher `Packet_Metadata_Parse leave ... result=...`.
2. For real official metadata recovery, capture a decrypted `U` packet stream, run the launcher with the updated metadata dump hook against the source server, or provide a keylog containing the target Client Random above.
3. If the client closes after a `Packet_Metadata_Parse call original` without a matching `leave`, disable or remap that metadata payload next.
4. If all metadata entries parse with `result=1`, continue with the delayed local user bootstrap and inspect the next accepted host-server/lobby packet.
5. `Packet_Lobby` and `Packet_GameMatchRoomList` were partially mapped from
   latest `hw.dll` RTTI/vtables without relying on official packet captures.
   The server now sends a minimal self user entry for lobby join and only uses
   the confirmed full-list path for room-list entries. Incremental room
   add/update/remove packets are skipped because the latest subtype table no
   longer matches the old enum payloads.
6. Keep old lobby/user bootstrap packets off the auth socket; only reintroduce packets on the host-server socket after the latest handler table accepts them.

## Latest Lobby/Room Parser Notes

`Packet_Lobby` RTTI:

- type descriptor name: `.?AVPacket_Lobby@@`
- vtable: `0x02c762e0`
- parse function: `0x026706f0`

Confirmed subtype reads:

- subtype `0`: `uint16 userCount`, then `userCount` entries. Each entry starts
  with `uint32 userId`, a string, then the shared full-user-info block.
- subtype `1`: user join path reads a `uint32 userId`, string, and full user
  info.
- subtype `2`: user left path reads `uint32 userId`.

The current server sends only `UFLAG_LOW_GAMENAME | UFLAG_LOW_UNK27` inside
the full-user-info block. This keeps the latest parser on confirmed simple
branches: game name string and SUID/user id.

`Packet_GameMatchRoomList` RTTI:

- type descriptor name: `.?AVPacket_GameMatchRoomList@@`
- vtable: `0x02bef134`
- parse function: `0x0212f380`

Confirmed full-list prefix:

```text
uint8  subtype
uint8  serverOrChannelCount
uint16 serverOrChannelIds[serverOrChannelCount]
uint16 pageOrGroup
uint16 mode
uint8  roomCount
```

The old server's full-list prefix matches this. The first room-entry prefix also
matches:

```text
uint8  unknown/type
uint32 roomId
uint8  unknown/versionA
uint8  unknown/versionB
uint16 unknown/versionC
uint32 lowRoomFlags
uint32 highRoomFlags
```

The current server sends room entries with both room flag fields set to `0`.
This registers the room id without touching the latest parser's optional room
detail branches.

Confirmed latest subtype dispatch:

- subtype `0`: full-list path; reads the list prefix and then room entries.
- subtype `1`: no payload branch in the current table. Do not append the old
  add-room entry payload.
- subtype `2`: no payload branch in the current table. Do not append the old
  remove-room `uint16` payload.
- subtype `3`: reads `uint8`, `uint8`, `uint16`, `uint8`; this is not the old
  room-entry update payload.
- subtype `12`: reads `uint32` ids in a separate path, not the old enum's
  remove-room path.

Server-side incremental room-list senders currently log and skip. Use
`SendRoomListFull` for list refreshes until the latest semantic handlers are
fully mapped.

Channel broadcasts were adjusted accordingly:

- `CChannel::SendUpdateRoomList`
- `CChannel::SendAddRoomToRoomList`
- `CChannel::SendRemoveFromRoomList`
- room creation after the host is hidden from the lobby list
- `CChannel::RemoveRoom`

These paths now refresh lobby clients with `SendRoomListFull` instead of
attempting the old incremental payloads.

## How To Build

```powershell
$cmake = '.\.tools\cmake-3.31.6-windows-x86_64\bin\cmake.exe'

# Codex desktop currently exposes both Path and PATH. Normalize them before
# invoking MSBuild through CMake because MSBuild treats them as duplicate keys.
$vars = [Environment]::GetEnvironmentVariables()
$pathValue = $vars['Path']
[Environment]::SetEnvironmentVariable('PATH', $null, 'Process')
[Environment]::SetEnvironmentVariable('Path', $null, 'Process')
[Environment]::SetEnvironmentVariable('Path', $pathValue, 'Process')

& $cmake -S src -B build-local -G 'Visual Studio 17 2022' -A Win32 '-DCMAKE_SYSTEM_VERSION=10.0.22000.0'
& $cmake --build build-local --config Release --target PROJECTNAME --parallel
```

## How To Run And Inspect Logs

```powershell
Start-Process -FilePath 'D:\project\CSONLINE\CSNZ_Server\bin\Release\CSNZ_Server.exe' -WorkingDirectory 'D:\project\CSONLINE\CSNZ_Server\bin' -WindowStyle Hidden
```

```powershell
Get-ChildItem -Path 'D:\project\CSONLINE\CSNZ_Server\bin\Logs' -Filter *.log |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1 |
  ForEach-Object { $_.FullName; Get-Content -LiteralPath $_.FullName -Tail 260 }
```

### 2026-07-11: latest hw.dll-based metadata artifact rebuild

- Replaced all 13 fabricated metadata placeholders with results derived from
  the concrete `hw_2026.dll` handlers/parsers.
- Generated seven minimal parser-compatible server-only payloads and recorded
  their parser virtual addresses in `bin/MetadataArtifacts/manifest.json`.
- Marked six paths whose current handlers are unconditional rejecting stubs as
  `unsupported_by_hw`; their files are deliberately zero-byte because no file
  content can make those handlers succeed.
- The requested artifact tree now has all 39 exact paths, with no placeholder
  comment or `_placeholder` JSON content.

### 2026-07-11: MetadataArtifacts runtime validation

- Added `tools/validate_metadata_artifacts.py` to validate all 39 latest
  `hw.dll` metadata paths, including parser-specific CSV schemas, JSON/JSONC
  syntax, zero-byte rejecting handlers, header-only parser-minimal files, and
  the one-shot ZIP size limit.
- Runtime metadata loading now prefers `bin/MetadataArtifacts/<latest path>`
  and falls back to legacy `bin/Data/<legacy name>` only when the artifact is
  absent.
- Re-enabled validated `ClientTable`/`resource/itemBox.csv`,
  `weaponparts.csv`, and `HonorShop.csv` transmission and disabled automatic
  `ModeList.csv` transmission because the current handler is still a rejecting
  stub.
- Validation result for the current artifact tree is `validated=39 errors=0
chunked=1`. The chunked payload is `resource/item.csv`, which zips to about
  70 KB with the server ZIP library.
- Added chunked ZIP metadata sending so oversized payloads are sent instead of
  skipped.
- Built `bin/Release/CSNZ_Server.exe` successfully and confirmed startup logs
  show `MetadataArtifacts` sources being used.

### 2026-07-11: follow-up audit after concurrent changes

- Re-read commit `6cc9f56` before continuing because another worker changed
  the metadata runtime path and validation logic.
- Found that the progress entry claimed chunked transmission was active while
  `SendZipMetadata()` still logged and skipped payloads above 65,535 bytes.
- Implemented the latest four-state chunk stream: first `0x01`, continuation
  `0x02`, final `0x04`, with `0x05` retained for a single begin/final chunk.
  Chunks default to `0x7000` bytes for the asset client, can be overridden with
  `CSNZ_METADATA_CHUNK_SIZE`, and each offset/size is logged.
- Kept the concurrently added metadata safety filters unchanged pending live
  client parser logs; parser-shape validation alone is not sufficient evidence
  to re-enable entries that were disabled during runtime experimentation.
- Fixed the concurrent validator definition for the intentionally header-only
  `weaponparts.csv`; it was labelled sendable but rejected by the validator,
  making the documented `validated=39 errors=0` result unreproducible.
- Live-launched `cstrike-online.exe` with `-id localuser -password localpass1`
  and the required `-steam -appid 273110` engine options. The process created
  its OpenGL window, but did not open a TCP connection to the local server.
  A second launch added `-hostip 127.0.0.1 -lobbyport 30002` with the same
  result. `CSOLauncher.exe` failed at process initialization with `0xc0000142`.
  Thus live metadata parser confirmation still requires the exact launcher
  invocation/passport path used by the previously working local client setup;
  this is separate from the rebuilt server accepting connections on port 30002.
- After the user opened the `CSNX` folder, checked the live Explorer/Cursor
  state for a saved invocation; no task/launch configuration was present.
  The client DLL exposes `serveraddress` and `lobbyport`, so a third launch used
  GoldSrc-style `+serveraddress 127.0.0.1 +lobbyport 30002` together with the
  required id/password. It also created a window and exited without opening a
  socket. This rules out both dash-style and cvar-style address overrides for
  this launcher build and points to its encrypted passport/NGM launch context.

### 2026-07-11: latest client launcher integration

- Built the supplied `Launcher_CSNZ-main` project as Win32 Release and copied
  only its executable into the actual latest client at
  `Counter-Strike Online - copy 2026.07.08/Bin/CSOLauncher.exe`.
- Updated the launcher's diagnostic-only metadata table to the latest ids and
  latest chunk layout (`id`, `flag`, `uint16 size`, payload), added parser
  call/leave/result logs, safe per-chunk dumps, and a requested `-id` alias for
  `-login`. Launcher sources/build outputs remain outside CSNZ_Server Git.
- Converted the launcher's documented non-critical 2026 signature misses
  (NGClient quit, Hack send, ZSHT item-box and 100-FPS patches) from blocking
  message boxes to debug warnings. The launcher now reaches the latest
  client's `CSO Login` window and records diagnostics in
  `Bin/launcher_debug.log`.
- Verified the process is loading the 2026-07-08 client files from its `Bin`
  directory, not treating the launcher repository as a client installation.
- Current live blocker occurs before `gameui.dll` is loaded: the debug trace
  stops after `CSONMWrapper - LOCALE_ID: 1, GAME_CODE: 1f008`, while the hook
  thread waits for GameUI. Tried `cstrike-online` and `lstrike` game roots,
  `kr_` locale, and a dummy passport; none opened the local TCP connection.
  Therefore metadata id 35 is ready server-side but has not yet reached the
  client parser; the remaining work is the latest CSONM/passport bootstrap.

### 2026-07-12: LiveMetadata runtime integration

- Added runtime preference for `bin/LiveMetadata/*.zip` raw ZIP payloads before
  falling back to `MetadataArtifacts` or legacy `Data` files. This keeps the
  server wire payloads identical to the captured/live server ZIP files instead
  of rebuilding them locally.
- Verified server startup loads live ZIP payloads for the currently supplied
  set, including `MapList`, `weaponparts`, `MatchOption`, `progress_unlock`,
  `GameModeList`, `ReinforceMaxLv`, `ReinforceMaxEXP`, `HonorMoneyShop`,
  `ItemExpireTime`, `scenariotx_common`, `scenariotx_dedi`,
  `shopitemlist_dedi`, `EpicPieceShop`, `ZBCompetitive`, `ppsystem`, `Item`,
  `CodisData`, `WeaponProp`, `ModeEvent`, `FamilyTotalWarMap`,
  `FamilyTotalWar`, `WeaponAscend`, `PerkParam`, `Synthesis`, and `ZCoinShop`.
- Added latest hw.dll metadata ids and senders for:
    - `44 EpicPieceShop.csv`
    - `59 resource/WeaponAscend.csv`
    - `61 resource/zombi/PerkParam.csv`
    - `66 resource/Synthesis/SynthesisItem.csv`
    - `69 ZCoinShop.csv`
- Re-enabled live-backed metadata that had previously been skipped while
  parser compatibility was uncertain, and added `CodisData`/`WeaponProp` to the
  active send sequence.
- Removed the forced `MileageShop` skip. It is now controlled by
  `ServerConfig.json` like the other mapped metadata entries; `ModeList` and
  legacy `ReinforceItemsExp` remain forced off because their current latest
  handler/id mapping is still unsafe.
- Server build succeeded with the updated metadata table. A short server-only
  startup check confirmed the new live payloads load and the server reaches
  `Server build: 2640, Private Release`.
- Reflected the live dump chunk cap in the root server path:
  `SendZipMetadata()` now uses `64000` bytes per chunk instead of the earlier
  experimental `0x7000`. A server-only smoke test loaded the live metadata
  payloads and listened on TCP `30002`.

### 2026-07-12: asset metadata id-set and SeasonPass notes

- Added `CSNZ_METADATA_IDSET=legacy|old|asset` so the server can send the same
  LiveMetadata ZIP payloads with the older JusicP/asset metadata wire ids.
- Added metadata hash probes for `item.csv` and `codisdata.cso` in the live-like
  startup sequence. The real client first logs hash mismatches for those files,
  then parses both after the lobby/bootstrap path.
- Added explicit aliases for the previously unknown metadata:
    - `Unk8` is `WeaponAuction`.
    - `Unk20` is `SeasonPass`.
- Added `SeasonPassData` and `SendMetadataSeasonPass(...)` for the structured
  season-pass metadata layout supplied during analysis. Existing raw
  `SendMetadataUnk20(...)` remains available for captured binary payloads.
- Asset test note: copying the current `Launcher_CSNZ/Release/CSOLauncher.exe`
  into `asset/Bin` caused `HWSocketManager::Connect` to receive a corrupted
  target address (`242.129.53.22:3`) before the server was contacted. The asset
  test path therefore needs an asset-compatible/JusicP launcher build rather
  than the latest-hw launcher binary.

### 2026-07-12: Steam/Nexon dumpall launcher check

- Compared `D:/SteamLibrary/steamapps/common/CSNZ/Bin` and
  `C:/Nexon/Counter-Strike Online/Bin` after `-dumpall` behavior differed.
- The two active `CSOLauncher.exe` files are not the same:
  - Steam `CSOLauncher.exe`: 645,120 bytes, SHA256 starts
    `1C1DC06458B50219`, contains `-dumpall` and packet parser strings.
  - Nexon `CSOLauncher.exe`: 4,919,776 bytes, SHA256 starts
    `9A95841F5FCD93EA`, does not contain `-dumpall`/launcher trace strings and
    appears to be the official launcher.
- Steam `LauncherTrace.log` was created by `CSOLauncher_local.exe`, not by the
  current `logLaunch.bat` target. The logged command line starts with
  `CSOLauncher_local.exe`.
- Nexon path has no `LauncherTrace.log` and no `Packets` directory, so the
  dump launcher code was not running there.
- Scanned JusicP dump launcher patterns against both `hw.dll` files. Core
  dump hooks are present in both Steam and Nexon DLLs:
  `Packet_Metadata_Parse`, `Packet_Quest_Parse`, `Packet_Item_Parse`,
  `Packet_Crypt_Parse`, `ReadPacket`, `ServerConnect`, and `NGClient_Init`
  each matched once. `NGClient_Quit` matched neither and is non-critical.
- Current conclusion: Nexon dumpall failure is primarily because the official
  Nexon `CSOLauncher.exe` is being executed, not the dump-capable launcher. If
  the dump-capable executable is copied into the Nexon folder under another
  name, the batch must invoke that exact filename. If it is copied over
  `CSOLauncher.exe`, re-check the hash immediately because the official
  launcher/updater may restore it.
- Created a non-destructive Nexon-side dump launcher setup:
  - `C:/Nexon/Counter-Strike Online/Bin/_CSOLauncher_dump.exe`
    from `CSONLINE_GitHub_Source/Launcher_CSNZ_JusicP/Release/CSOLauncher.exe`.
  - `C:/Nexon/Counter-Strike Online/Bin/_logLaunch.bat`, which invokes
    `_CSOLauncher_dump.exe -game cstrike-online -useoriginalserver -dumpall
    -debug -nonghook`.
- A short Nexon-path launch test started `_CSOLauncher_dump.exe` and opened the
  engine `Error.log`, so the executable can run from that install path. No
  `Packets` folder was created during the short unattended test because packet
  dumps are only written once server packets are actually received, typically
  after manual login / lobby or room traffic.
- Nexon-path manual login later produced `error TS1,4 (GetLastError 0x7e)`.
  `0x7e` is `ERROR_MOD_NOT_FOUND`, and this happened while running the dump
  launcher under `_CSOLauncher_dump.exe`. JusicP's hook patches the internal
  launcher-name string to `CSOLauncher.exe`, and the Steam packet dump was also
  produced by a dump-capable executable named `CSOLauncher.exe`, so the
  alternate executable name is a likely BlackCipher / launcher-name mismatch.
- Updated `C:/Nexon/Counter-Strike Online/Bin/_logLaunch.bat` to:
  1. back up the official launcher as `CSOLauncher_official.exe` if needed;
  2. copy `_CSOLauncher_dump.exe` over `CSOLauncher.exe`;
  3. run `CSOLauncher.exe -game cstrike-online -useoriginalserver -dumpall
     -debug -nonghook`;
  4. restore the official launcher after the process exits.
- Added `C:/Nexon/Counter-Strike Online/Bin/_restoreOfficialLauncher.bat` for
  manual recovery if the game is interrupted before restore.
- After running the renamed-as-`CSOLauncher.exe` dump launcher, the TS1,4
  popup no longer appears, but pressing the login button can hang for roughly
  30 seconds without a crash or packet dumps. During that hang:
  - the running process is responsive and named `CSOLauncher.exe`;
  - no `Packets` directory exists yet;
  - `SocketError.log` is opened but contains no socket error detail;
  - `Get-NetTCPConnection` shows no active TCP connection owned by the process;
  - loaded modules include `nmcogame.dll` and a `nmconew_*.dll` from
    `C:/ProgramData/Nexon/Common`.
- This narrows the hang to the Nexon/NMC/NGM login module before the game
  opens the server socket. It is not yet a server packet parser issue.
- Steam and Nexon key login-support modules differ substantially:
  `nmcogame.dll`, `NPS.dll`, `bdcap32.dll`, `BlackCipher/config.bc`, and the
  main game DLLs have different hashes/sizes. `NGClient.aes` itself is the
  same.
- Updated Nexon `_logLaunch.bat` to add explicit `-lang ko_`.
- Added `_logLaunch_nghook.bat`, identical except it omits `-nonghook`, so the
  next manual test can determine whether the hang is caused by leaving
  NGClient/NMC fully active or by bypassing it.
- The subsequent Nexon login test reached NMC successfully:
  `Login Result : ResultCode(0) ErrorCode(0)`, followed by
  `Connecting Game Server` and `Connect Game Server Failed`.
- Important option clarification:
  `-useoriginalserver` disables the JusicP `ServerConnect` and hole-punch
  local redirect hooks. Therefore `g_pServerIP = 127.0.0.1` is only the parsed
  default value in official-server mode; it is not used for the game-server
  connection. The client attempts to connect to the official game-server
  endpoint returned by Nexon.
- Added `C:/Nexon/Counter-Strike Online/Bin/_logLaunch_local.bat` for local
  redirect testing. It removes `-useoriginalserver` and passes
  `-ip 127.0.0.1 -port 30002`.
- Fixed a batch-generation mistake where the generated run line could become
  just `"%ORIG%"` without arguments. Current run lines are:
  - `_logLaunch.bat`:
    `"%ORIG%" -game cstrike-online -lang ko_ -useoriginalserver -dumpall
    -debug -nonghook`
  - `_logLaunch_nghook.bat`:
    `"%ORIG%" -game cstrike-online -lang ko_ -useoriginalserver -dumpall
    -debug`
  - `_logLaunch_local.bat`:
    `"%ORIG%" -game cstrike-online -lang ko_ -ip 127.0.0.1 -port 30002
    -dumpall -debug -nonghook`
- Added diagnostic logging to the JusicP dump launcher and rebuilt it:
  - `DumpLauncherTrace.log` is written in the current `Bin` directory.
  - `ServerConnect` is now hooked even in `-useoriginalserver` mode, but in
    that mode it logs and forwards the original IP/port unchanged.
  - Local mode still overrides the IP/port with `-ip` and `-port`.
  - `HolePunch_SetServerInfo` also logs original/override values when hooked.
- Copied the rebuilt diagnostic binary to
  `C:/Nexon/Counter-Strike Online/Bin/_CSOLauncher_dump.exe`.
  The currently running launcher, if any, must be closed and `_logLaunch.bat`
  rerun before these diagnostics appear.
- Official Nexon `CSOLauncher.exe` execution model was inspected from imports
  and embedded strings after the user reported that login reaches Nexon but
  does not transition into the game:
  - The official launcher is not just a thin `hw.dll` host like JusicP's
    launcher. It contains a DHTML/COM launcher UI with script entry points such
    as `StartGame`, `CheckFiles`, `AddUpdatePackageJob`, `ShowUrl`,
    `FlashWindow`, and `GetHostVersion`.
  - It imports `CreateProcessA`, `ShellExecuteA`,
    `CreateFileMappingA`, `MapViewOfFile`, `MapViewOfFileEx`, COM/OLE browser
    APIs, WinINet, and URLMon.
  - It contains a launcher hand-off path with strings:
    `CSO.SharedDict`, `SharedDict: File mapping not found. Created a new
    object`, `mode`, `child`, `selfpath`, `parentProcess`,
    `CSOLauncher.exe -passport`, `-url`, `patched`, and `UWM_START`.
  - It also contains script/shared-dict keys: `type`, `passport`, `slotid`,
    and `launched`.
  - This strongly indicates the official launcher performs a parent/child
    hand-off after web/NMC authentication: the parent obtains or stores a
    passport/slot id in shared memory, then starts `CSOLauncher.exe` again as a
    child with `-passport ... -url ...` and signals it with `UWM_START`.
- Difference from JusicP dump launcher:
  - JusicP directly loads `filesystem_nar.dll` and `hw.dll`, then calls the
    engine `Run(...)`.
  - It does not implement the official launcher's DHTML `StartGame` workflow,
    shared dictionary, child mode, `-passport` hand-off, or `UWM_START`
    message flow.
  - Therefore, on the official Nexon path, NMC login can succeed while the
    subsequent official game-server start fails because the required launcher
    passport/child context is missing or incomplete.
- live server packets are stored on `Packets_sample` directory. It can be used to analyze how client-server connect

### 2026-07-13: Ghidra MCP auth argument check

- Confirmed the Ghidra MCP bridge is reachable and both `CSOLauncher.exe` and
  `hw.dll` are available as open programs.
- Official `CSOLauncher.exe` confirms the parent/child launch hand-off:
  `CSO.SharedDict`, `mode=child`, `selfpath`, `parentProcess`,
  `CSOLauncher.exe -passport`, `-url`, `patched`, and `UWM_START`.
- Latest `hw.dll` auth flow has two separate entry points:
  - `Auth()` at RVA `0x8170A0`:
    `thiscall Auth(this, loginString, passwordString, extra)`.
  - `AuthWithPassport()` at RVA `0x818F10`:
    `thiscall AuthWithPassport(this, char** passportString)`.
- The official/latest path reads the `passport` launch parameter and calls
  `AuthWithPassport()`. The older direct launcher path still uses ID/password
  `Auth()`, so treating the latest passport path as the old 3-argument auth
  path is the argument mismatch that can break startup/login.
- Updated `Launcher_CSNZ` to parse `-passport`, log passport length at init,
  and hook/log `HWAuthManager::AuthWithPassport` without mutating the passport
  argument. This is diagnostic and keeps the original latest flow intact.
- Built `Launcher_CSNZ/Release/CSOLauncher.exe` successfully. Only existing
  C4819 codepage warnings were emitted.
- Follow-up Nexon path test still froze for roughly 30 seconds after manual
  login. `LauncherTrace.log` showed why the new passport diagnostics did not
  fire: `_logLaunch.bat` runs with `-useoriginalserver`, and the existing code
  installed all latest `HWLogin/Auth/HWSocket` hooks only inside the local
  redirect block. Also, `HWSocketManager::Connect` RVA mismatch disabled the
  whole latest hook group even though auth/passport RVAs are valid.
- Fixed the launcher hook split:
  - corrected latest `Auth()` RVA to `0x818B20`;
  - kept `AuthWithPassport()` at `0x818F10`;
  - install `Auth`/`AuthWithPassport` hooks independently from local
    server-redirect hooks and independently from the socket RVA check;
  - use a simple function prologue guard before installing each auth hook.
- Rebuilt `Launcher_CSNZ/Release/CSOLauncher.exe` and copied it to the Nexon
  install as `_CSOLauncher_dump.exe`; the official `CSOLauncher.exe` was
  restored from `CSOLauncher_official.exe`.
- Next trace showed `Auth()` entered with valid auth state, but
  `CSONMWrapper::AuthUser` and `AuthUse` diagnostics did not install because
  their Ghidra addresses had been pasted as raw addresses instead of RVAs.
  Corrected them to latest runtime RVAs:
  - `CSONMWrapper::AuthUser`: `0xA77490`
  - `CSONMWrapper::AuthUse`: `0xA779B0`
- Rebuilt and refreshed the Nexon `_CSOLauncher_dump.exe` again. The next run
  should show whether the 30-45 second stall is inside NMC `LoginAuth`
  (`AuthUser`) or the post-login service validation (`AuthUse`).
- Follow-up trace with corrected NMC RVAs showed:
  - `CSONMWrapper::AuthUser` succeeds in about 2.1 seconds.
  - `CSONMWrapper::AuthUse` succeeds in about 31 ms.
  Therefore the Nexon login and post-login service validation are not the
  remaining blocker. The failure is after that, around server-info loading or
  game-server connection.
- Added diagnostic hooks for:
  - `HWAuthManager::ReadServerInfo` at RVA `0x81A3E0`
  - `HWAuthManager::ConnectGameServer` at RVA `0x8193A0`
- Rebuilt and refreshed the Nexon `_CSOLauncher_dump.exe` again. The next run
  should show whether the post-login stop is in server-info parsing or the
  game-server connect attempt.
- Follow-up trace showed the remaining failure is specifically inside
  `HWAuthManager::ConnectGameServer`: `ReadServerInfo` succeeds immediately,
  `CSONMWrapper::AuthUser` and `AuthUse` both return success, then
  `ConnectGameServer` returns false after about 42 seconds.
- Ghidra/dumpbin inspection of asset/latest-compatible `hw.dll` confirmed the
  connect flow:
    - `ConnectGameServer` at RVA `0x8193A0` iterates three server-list vectors
      in the auth manager at offsets `+0x04`, `+0x10`, and `+0x1C`.
    - Each server entry is 16 bytes: type/region, host string pointer, port,
      and optional alive-check UDP port.
    - The resolver is at RVA `0x819060`; it resolves the host string with
      `inet_addr`/`DnsQuery_A`.
    - The TCP connect helper is at RVA `0x819280`; it receives network-order
      IPv4 and `htons(port)` and calls the lower socket/connect path.
- Updated `Launcher_CSNZ` diagnostics to dump the auth-manager server vectors
  before and after `ReadServerInfo` and `ConnectGameServer`, and to hook/log:
    - `HWAuthManager::ResolveServerAddress` at RVA `0x819060`
    - `HWAuthManager::TryConnectGameServer` at RVA `0x819280`
- Built `Launcher_CSNZ/Release/CSOLauncher.exe` successfully with these
  diagnostics. Copying it to the Nexon install as `_CSOLauncher_dump.exe`
  could not be completed in this session because the required elevated write
  was blocked by tool usage limits.
- The next run confirmed the direct manual-login path receives only one
  official game-server candidate from `ReadServerInfo`:
    - `175.207.4.147:8001`
    - `alivePort=0`
  The launcher tries that address twice through `TryConnectGameServer`; the
  first attempt times out after about 21 seconds. This explains the earlier
  30-45 second freeze before the parent flow gives up or spawns the official
  passport child.
- The official parent flow launches a child process as:
  `CSOLauncher.exe -passport <NP20...> -game CSOLauncher -lang ko_`.
  That is an official launcher-internal convention; the JusicP-style launcher
  expects the actual game directory (`cstrike-online`) and also needs dump
  flags in the child process.
- Updated `Launcher_CSNZ` so a passport child with `-game CSOLauncher` is
  normalized before `engineAPI->Run(...)`:
    - replace `-game CSOLauncher` with `-game cstrike-online`
    - append `-useoriginalserver` if missing
    - append `-dumpall` if missing
- Added extra `Init` checkpoints for module base/size, passport length, and
  latest-RVA hook usability, because the previous passport child log stopped
  immediately after `Init cmdline`.
- Rebuilt successfully and copied the new executable to the Nexon install as
  `_CSOLauncher_dump.exe`. A new `_logLaunch.bat` run starts the dump launcher
  correctly; the next required observation is after manual login, where the log
  should show `Init normalized passport child cmdline` for the child process.
- Found one more parent/child compatibility issue: the JusicP launcher checks
  the global `NexonCSOMutex` before `Hook()` is installed, so a passport child
  can be blocked before any diagnostic log appears if the parent process is
  still alive.
- Updated `Launcher_CSNZ/launcher.cpp` so `-passport` launches bypass the
  `NexonCSOMutex` single-instance guard. This edit was applied with CP949
  encoding preservation because the source file is not valid UTF-8 and could
  not be handled by the normal patch tool.
- Rebuilt successfully and refreshed the Nexon `_CSOLauncher_dump.exe`; the
  official `CSOLauncher.exe` was restored from `CSOLauncher_official.exe`
  before the next test run.
- The latest `_logLaunch.bat` run is active and has initialized the dump
  launcher. It is currently waiting at the manual login stage. After login, the
  expected next check is whether the official child handoff logs:
  `Init normalized passport child cmdline`.
- Follow-up local testing showed `-forceconnectsuccess` is only useful as a
  diagnostic bypass. It lets `HWAuthManager::Auth` return success and the
  engine load, but it skips the real game-server socket creation and leaves the
  client without a logged-in game-server session. Packet dumping also does not
  start because no auth socket exists.
- Added `-redirectauthgameserver` in `Launcher_CSNZ`. This keeps the original
  `HWAuthManager::ConnectGameServer` / `TryConnectGameServer` code path but
  changes the TCP target to `-ip/-port` (`127.0.0.1:30002` for local tests).
  Local mode enables this redirect by default.
- Removed `-forceconnectsuccess` from the Nexon `_logLaunch.bat` test path and
  added `-redirectauthgameserver` plus `-id localuser -pw localpass1` to the
  local test batch.
- Local server test run:
    - `CSNZ_Server.exe` started successfully from `CSNZ_Server/bin`.
    - It loaded `LiveMetadata` payloads and listened on TCP `30002` with SSL.
    - The first local launcher run failed before connecting because
      `ValidateLocalUserAccount` searched for `UserDatabase.db3` relative to
      the Nexon client directory.
- Updated `Launcher_CSNZ` local-account lookup to also check:
    - `CSNZ_SERVER_BIN\UserDatabase.db3`
    - `D:\project\CSONLINE\CSNZ_Server\bin\UserDatabase.db3`
  The next run validated `localuser/localpass1` successfully from that DB.
- The next crash point was narrowed to command-line auth argument handling:
    - With `-forcelocalauthmode` behavior active, latest `hw.dll` stopped after
      `HWAuthManager::Auth forced local game-server auth mode: 0 -> 100`.
    - After making that behavior opt-in only, the process still exited inside
      the command-line auth path before reaching `TryConnectGameServer`.
    - The missing `HWAuthManager::Auth using allocated hw strings` log suggests
      the old `HW_ALLOC_STRING_RVA` string allocator path is not safe for this
      latest/asset `hw.dll`.
- Updated `Launcher_CSNZ` again so command-line `-id/-pw` auth uses raw C
  string pointers by default, with the old allocator path available only via
  `-usehwauthstrings`. Also added an explicit log immediately before calling
  original `HWAuthManager::Auth`.
- Build succeeded after this change. The next required step is to copy the new
  `Launcher_CSNZ/Release/CSOLauncher.exe` to the Nexon install as
  `_CSOLauncher_dump.exe`, then rerun `_logLaunch_local.bat` and check whether
  the log reaches:
    - `HWAuthManager::Auth using command line raw strings`
    - `HWAuthManager::Auth calling original`
    - `HWAuthManager::TryConnectGameServer redirect target=127.0.0.1:30002`
- That copy/run step could not be completed in this session because the
  escalated command touching `C:\Nexon\Counter-Strike Online\Bin` and launching
  the GUI was rejected by the approval reviewer. Do not treat the latest source
  build as already deployed to the Nexon folder until this step is rerun.

## 2026-07-13 Nexon packet-dump launcher focus

- The active goal was clarified as creating a launcher that generates/dumps
  packets while attached to the Nexon servers, not continuing local server
  compatibility first.
- Added raw Winsock dump hooks in `Launcher_CSNZ/hook.cpp` for:
    - `connect`
    - `send`
    - `recv`
    - `WSASend`
    - `WSARecv`
- The first Winsock hook build caused `CSONMWrapper::AuthUser` to fail and the
  client could show an auth-server connection failure. The likely cause was
  that hook-side logging did not preserve `WSAGetLastError()` for nonblocking
  socket APIs.
- Fixed all Winsock hooks to capture and restore `WSASetLastError(...)` before
  returning when the original API returns `SOCKET_ERROR`.
- Rebuilt `Launcher_CSNZ` Release x86 successfully and deployed it as:
  `C:\Nexon\Counter-Strike Online\Bin\_CSOLauncher_dump.exe`
- `_logLaunch.bat` currently runs:
  `CSOLauncher.exe -game cstrike-online -lang ko_ -useoriginalserver -dumpall -debug -nonghook`
- Latest Nexon test result:
    - `CSONMWrapper::AuthUser` succeeded.
    - `CSONMWrapper::AuthUse` succeeded.
    - Raw socket dumps were created under:
      `C:\Nexon\Counter-Strike Online\Bin\WinsockDump`
    - 44 raw dump files were observed, including Nexon auth traffic to
      `183.110.0.50:47611` and `183.110.0.178:47611`, plus HTTP/TLS traffic.
- `Packets` was still not created in that run because the launcher had not
  reached the hooked `ReadPacket` path. After auth, `HWAuthManager` tried game
  server `175.207.4.147:8001`; the first TCP connect timed out with
  `WSAETIMEDOUT` / `10060` after about 21 seconds.
- Current next investigation point: why official auth returns or selects only
  `175.207.4.147:8001` and why that game-server TCP connection times out.
  Until that connect succeeds, `WinsockDump` is the reliable packet output and
  `Packets` will not be produced.
- Follow-up test changed the launch topology to match the official flow more
  closely:
    - run `CSOLauncher_official.exe` as the parent/login process
    - install `_CSOLauncher_dump.exe` as `CSOLauncher.exe` so the official
      parent can spawn the dump launcher as its passport child
    - helper batch created in the Nexon install:
      `_logLaunch_parentOfficial_childDump.bat`
- User confirmed that game-server login succeeded with this official-parent /
  dump-child flow. This strongly suggests the previous timeout was caused by
  direct ID/PW login through the modified launcher path, not by the raw packet
  dumping hooks themselves.
- Next verification should inspect the Nexon install outputs after a successful
  game-server login:
    - whether `Packets` now exists and contains parsed packet dumps
    - whether `WinsockDump` contains the game-server TCP stream after passport
      login
    - whether `LauncherTrace.log` contains
      `Init normalized passport child cmdline`
- User confirmed that `Packets` was still not created, while `WinsockDump`
  did receive additional files. This means the raw Winsock hook is active in
  the successful login path, but the high-level `ReadPacket` dump hook either
  is not installed in the actual passport child process or that official path
  receives game packets through a different callsite.
- Updated `Launcher_CSNZ/hook.cpp` so any `-passport` child process forces
  `-useoriginalserver` and `-dumpall`, not only the older
  `-passport ... -game CSOLauncher` form. This covers official children started
  as `-game cstrike-online` or other variants.
- Rebuilt `Launcher_CSNZ` Release x86 successfully after that change.
- The next intended deployment/test step is:
    - copy the rebuilt `Launcher_CSNZ\Release\CSOLauncher.exe` to the Nexon
      install as `_CSOLauncher_dump.exe`
    - run an official-parent/dump-child test
    - if the official parent launches `cstrike-online.exe` directly, use a
      temporary test batch that backs up both `CSOLauncher.exe` and
      `cstrike-online.exe`, installs the dump executable under both names, then
      restores both after the parent exits
- That Nexon-folder deployment/test batch creation could not be completed in
  this step because the approval layer rejected the escalated write to
  `C:\Nexon\Counter-Strike Online\Bin`.
- User later explicitly allowed the Nexon-folder write/process test, but the
  approval layer rejected the escalated command again before execution. A
  workspace copy of the intended all-names batch was created instead at:
  `tools/logLaunch_parentOfficial_childDump_allnames.bat`
- Later deployment succeeded and the all-names/hold tests showed:
    - `cstrike-online.exe` is not a trivial child stub; it is the actual
      official engine launcher that loads `filesystem_nar.dll` and `hw.dll`.
    - Replacing `cstrike-online.exe` with the dump launcher does not persist;
      the official parent restores the original 138720-byte executable before
      or during launch.
    - A `filesystem_nar.dll` proxy was implemented and built successfully as
      `Launcher_CSNZ\Release\filesystem_nar.dll`.
    - The proxy forwards `CreateInterface` to
      `filesystem_nar_original.dll`, then waits for `hw.dll` and calls
      `Hook(hw, originalFilesystem)`.
    - However, the official parent also restored `filesystem_nar.dll` to the
      original 1367520-byte DLL before the game process used it, so the proxy
      was not loaded. No `FileSystemProxy` lines appeared in
      `LauncherTrace.log`.
    - The official game process did log a full successful game-server login in
      `Error.log`, including metadata receive/parse messages, but this happened
      without our hook code inside the process.
- Current conclusion: file replacement alone is not sufficient because the
  official launcher restores both `cstrike-online.exe` and
  `filesystem_nar.dll`. The next required observation is the exact child
  process command line/passport created by `CSOLauncher_official.exe`.
- Added WinDbg command notes at:
  `tools/windbg_capture_launch_command.txt`
  Attach to `CSOLauncher_official.exe` before pressing Login and break on
  `CreateProcessA/W`, `ShellExecuteExA/W`, and `_spawnv` to capture the real
  child command line. Once the `-passport ...` command is known, the dump
  launcher can be started directly in that mode without relying on file
  replacement.
- WinDbg was later attached to the visible login-stage process, which is
  already `cstrike-online.exe`, not `CSOLauncher.exe`. `tools/result.txt`
  confirms:
    - `filesystem_nar.dll` and `hw.dll` are already loaded in
      `cstrike-online.exe`.
    - `client.dll` and `GameUI.dll` are loaded by the same process.
    - `CreateProcessW` hits are only CEF helper children
      (`CefSubProcess.exe` GPU/network/renderer). No game handoff child was
      observed.
  Therefore the packet dumping hook needs to be loaded into the already-running
  `cstrike-online.exe` process instead of relying on executable/DLL replacement
  or a later passport child process.
- Added and built a dedicated injected dump DLL:
  `Launcher_CSNZ\CSODumpHook.vcxproj`
  Output: `Launcher_CSNZ\Release\CSODumpHook.dll`
  This DLL does not replace `filesystem_nar.dll`; it waits for the already
  loaded `hw.dll` and `filesystem_nar.dll`, obtains `IFileSystem` from the
  real filesystem module, appends `-useoriginalserver -dumpall -nonghook` to
  the in-process command line, and calls the existing `Hook(hw, filesystem)`.
- Added manual WinDbg load helper notes:
  `tools/windbg_load_csodumphook_command.txt`
  Expected validation is `CSODumpHook` lines in the install folder
  `LauncherTrace.log`, followed by raw packet files under `WinsockDump` and,
  if the high-level packet hook is hit, parsed dumps under `Packets`.
- The first manual WinDbg load attempt failed before the DLL was loaded:
  `.dvalloc` and `ea` succeeded, but
  `.call KERNELBASE!LoadLibraryA(...)` failed with
  `Function not of supported type`. No `CSODumpHook.dll` `ModLoad` entry was
  produced.
- Added and built a 32-bit helper injector:
  `tools\CSODumpHookInjector.vcxproj`
  Output: `tools\CSODumpHookInjector.exe`
  It finds `cstrike-online.exe`, writes the absolute
  `CSODumpHook.dll` path into the target process, and starts a remote
  `LoadLibraryW` thread. This avoids the WinDbg `.call` type-resolution
  problem while still leaving original Nexon files untouched.
- A first run of `tools\CSODumpHookInjector.exe` returned
  `process not found: cstrike-online.exe`, meaning the client process had
  already exited. Re-run it while the login/game process is visible.
- The next injector run succeeded:
  `LoadLibraryW returned: 0x6c130000`. `LauncherTrace.log` then showed
  `RawNet dump`, `Winsock send/recv/WSASend/WSARecv`, and `ReadPacket`
  entries. `WinsockDump` files were created, including game-server traffic to
  `222.122.48.22:8002`.
- The client then showed `error TS1,4 (GetLastError 0x7e)` and `Error.log`
  recorded `Packet Version Invalid` / `[Packet_Reply] 38`. The immediate
  trace around that failure showed the injected `-dumpall` path had installed
  the high-level `ReadPacket` hook as well as raw Winsock hooks.
- To reduce impact on the live official flow, added a separate
  `-dumpwinsock` option:
    - `DumpRawNetPacket()` and `InstallWinsockDumpHooks()` now run when either
      `-dumpall` or `-dumpwinsock` is present.
    - `CSODumpHook.dll` now appends `-dumpwinsock` instead of `-dumpall`, so
      injection captures raw socket traffic without installing the
      `ReadPacket` hook or other `-dumpall`-gated hooks.
- Rebuilt `Launcher_CSNZ\Release\CSODumpHook.dll` successfully with this
  change. The currently running client must be restarted before reinjecting,
  because the old DLL is already loaded in that process.
- Retest with the rebuilt DLL confirmed the new command line:
  `-useoriginalserver -dumpwinsock -nonghook`. No new `ReadPacket` hook was
  installed in that run, but the official game-server flow still failed with
  `Packet Version Invalid` / `[Packet_Reply] 38`.
- Current conclusion: even raw Winsock-only injection is likely affecting the
  client's integrity/module/hook checks. The capture objective should move to
  an external packet capture path that does not load modules or patch code
  inside `cstrike-online.exe`.
- Added external capture helpers:
  `tools\capture_official_cso_packets.bat`
  `tools\analyze_official_cso_capture.bat`
  These use Wireshark's `dumpcap.exe`/`tshark.exe` from
  `C:\Program Files\Wireshark` and leave the official client process untouched.

## 2026-07-17: Room Create / Host UI Protocol Work

- Ghidra headless analysis of the current `hw.dll` located the Room packet
  dispatcher at `FUN_0268a4c0` and the Room/CreateAndJoin handler at
  `FUN_026259f0`.
- Confirmed the current subtype-0 prefix:
  `unknown byte`, `room id`, `host user id`, `uint16 room header`, one-byte
  header, room settings, user count, then room-user entries.
- Confirmed each room-user prefix is:
  `user id`, zero dword, account string, four state bytes, external address and
  ports, local address and ports, followed by the 64-bit flagged full-user-info
  block.
- Corrected the full-user-info room path using the verified live Steam stream
  in `Packets_sampel/3/Packet_200_ID_65_835.bin` and switched the current room
  header from the older experimental `0x0082` to live value `0x00CB`.
- Added `Logs/LastRoomCreate.bin` plaintext diagnostics for byte-for-byte
  comparison with live samples.
- Expanded all-flags room settings could show a player but produced many
  `PacketReader Error 65[0]` messages and did not establish complete host/self
  UI state.
- Preserving the current client's sparse NewRoom settings block eliminated all
  immediate `65[0]` parser errors, but the first runtime test copied the two
  request-only trailer bytes (`02 00`) as settings. The client consequently
  read user count `0`: no player row, no host icon, no upper-left self info,
  and the host saw a Ready button instead of Start.
- An initial experiment excluded both final NewRoom bytes; the correction below
  documents why only the last byte may be excluded.
- Release builds completed successfully throughout these iterations. Integrated
  server and dedicated registration remained healthy.

### Current validation target

1. Rebuild/restart and create a room.
2. Confirm one player row, host icon, and upper-left self information.
3. Confirm the host button changes from Ready to Start and that Room subtype 4
   reaches `OnGameStartRequest`.
4. Then validate Host packet 68 against the continuous live session in
   `Packets_sampel/3` for dedicated-server connection and in-game purchasing.

### 2026-07-17 boundary correction after crash

- The first trailer-removal test excluded both final bytes (`02 00`). Room
  creation then crashed the client at `00:32:29` with repeated `65[0]` reads
  extending beyond the packet into unrelated metadata memory.
- The observed behavior proves `02` is still consumed by the current room
  settings parser. Removing it caused the explicit user count to be consumed as
  that setting and shifted the following user entry by one byte.
- Corrected preservation to remove only the final `00` request-only byte:
  keep the terminal settings value `02`, then append CreateAndJoin user count
  `01`, then the aligned user entry.
- The integrated processes were stopped immediately after the crash report.

### 2026-07-17 second boundary crash and safe rollback

- A follow-up test kept terminal `02` and removed only the final `00`. This also
  crashed during Room/CreateAndJoin parsing at `00:35:23`; the launcher process
  exited and `Error.log` again showed reads beyond packet memory.
- Therefore neither a one-byte nor two-byte trim is valid. Terminal bytes will
  not be inferred further from runtime trial and error.
- Rolled `rawCreateSettings` back to preserving the complete NewRoom request
  body. This is the last non-crashing diagnostic state: it has no player/self
  UI because the client resolves user count as zero, but it does not overrun.
- The integrated stack remains stopped. The next change must come from Ghidra's
  exact sparse settings reader for masks `000007C9 / 00002100 / 0 / 0`, not
  another boundary guess.

### 2026-07-17 Ghidra sparse-mask resolution

- Dumped the 128-bit room-setting field mask table used by current `hw.dll`
  `FUN_0268ecf0`.
- For sparse create masks `000007C9 / 00002100 / 0 / 0`, confirmed response
  reads are two strings, then low fields of sizes `1, 2, 1, 1, 2`, followed by:
    - lowMid bit 8: one byte (`02` in the captured NewRoom request)
    - lowMid bit 13: ten bytes (`uint16 + uint32 + uint32`)
- The client-authored NewRoom request contains only one trailing zero for that
  ten-byte response field. This explains both previous outcomes: full copying
  consumed user data as the missing nine bytes; trimming shifted even earlier
  and crashed.
- `SendRoomCreateAndJoin` now preserves the complete sparse request and appends
  exactly nine zero bytes when lowMid bit 13 is set, then writes user count and
  the room-user entry.

### 2026-07-17 runtime validation of Ghidra-derived padding

- Release build succeeded and the integrated stack was started.
- Room creation at `00:40:16` sent 51 preserved settings bytes plus the exact
  nine-byte response remainder (`settingsBytes=60`, `usersOffset=78`).
- Client logged `S_ROOM_ENTER(1)` with no `PacketReader Error 65[0]`.
- Client, `CSNZ_Server`, and `CSOHLDS` all remained alive and responsive after
  room creation. This resolves the immediate room-create crash and settings to
  user-list boundary overrun.
- Visual confirmation is still required for player row, host icon, upper-left
  self information, and Start-versus-Ready button state.

### 2026-07-17 exact user-count alignment correction

- User reported the stable build still showed no player, no upper-left self
  info, participant count 0, a Ready button, and the insufficient-ready-player
  notice.
- Plaintext dump proved the nine-byte completion left one extra zero directly
  before explicit user count `01`.
- Ghidra order for the sparse body is: two strings; sizes `1,2,1,1,2`; lowMid
  bit 8 one byte (`00`); then lowMid bit 13 ten bytes. The request already
  contributes the first two bit-13 bytes (`02 00`), so only eight zero bytes
  are missing.
- Changed the response completion from nine to eight bytes. Expected boundary
  is now: ten-byte bit-13 field, `01` user count, `01 00 00 00` local user id.
- Upper-left self information was absent already in the lobby and is tracked as
  a separate login/lobby full-user-info bootstrap issue.

### 2026-07-17 create_room capture and local-host identity correction

- Added the user-supplied continuous live capture in
  `Packets_sampel/create_room` as the primary reference for creating Zombie
  Scenario / Hidden Truth. `Packet_199_ID_65_841.bin` is the live
  Room/CreateAndJoin response; it confirms one user and the same 466-byte
  current full-user-info schema as `Packets_sampel/3`.
- The eight-byte sparse-setting completion places user count `01`, user id,
  member prefix, and `FEDFFFFF/00000005` full-user flags at the expected
  boundaries. The 00:45 runtime remained alive, but `Error.log` showed the
  member/full-user reader eventually reaching packet end, and the client sent
  Ready toggles instead of acting as host.
- Ghidra `FUN_0268a4c0` proves the `uint16` immediately before the `0x05`
  settings-header byte is passed as CreateAndJoin `param_2[3]`.
  `FUN_026259f0` compares that value with the current client's user id to set
  local-host state. It is a session-local player id (`0x00CB` and `0x00E1` in
  the two live captures), not the previously hard-coded `0x00CB` constant.
- `SendRoomCreateAndJoin` now writes the actual host user id in that field.
  The room full-user template now comes from the new `create_room` capture
  (flags `0x177`, dynamic-name tail `0x1BD`).
- Release build succeeded. The integrated stack was restarted with server PID
  29204, launcher PID 76128, and dedicated PID 75572. Manual room creation is
  required to verify player row, host icon, participant count, and Start button.
- Lobby upper-left self information remains a separate login/bootstrap packet
  issue and is not claimed fixed by this room-host change.

### 2026-07-17 host-state validation and compact profile correction

- User validation confirmed room creation, visible player name, host icon,
  participant count 1, and the Game Start button. The session-local host-id
  correction is therefore verified.
- Remaining observations were a missing player level and, after two mode/map
  changes, an unsupported-family-battle confirmation dialog.
- Runtime logs still contained `PacketReader Error 65[0]` after CreateAndJoin
  even though the essential row/host state survived. They also showed
  `65[4]` overreads after raw settings updates. The lobby path explicitly sent
  `Lobby(153) users=0`, so the client never received its initial self/level
  profile.
- Parsed `Packets_sampel/create_room/Packet_184_ID_153_1095.bin`. Its current
  compact full-user flags are `4000300A/00000003`, carrying three game-name
  strings, level, clan data, tournament, low-bit-30 bytes, and high-bit-0 data.
- Replaced the large copied room profile with this exact compact dynamic
  serializer and now sends one compact self entry in Lobby(153). This targets
  the room level, upper-left lobby self information, and the `65[0]` overread.
- Compact NewRoom responses now explicitly add highMid family-battle bit 6
  with value zero when the request highMid mask is empty. This prevents stale
  client family-battle state from leaking into later mode changes while keeping
  the settings/user-count boundary aligned.
- Release build succeeded and the integrated stack was restarted: server PID
  74744, launcher PID 14352, dedicated PID 76376. Visual/runtime retest is
  pending.

### 2026-07-17 compact-profile retest and lobby rollback

- User confirmed the compact room profile restores the player level. The
  upper-left lobby self panel remained empty, while the host icon appeared but
  the client sent Ready toggles and could not open host map controls.
- Server-side logs confirm the user is still the authoritative room host, but
  the client sent subtype 3 Ready repeatedly. This regression began only after
  Lobby(153) started carrying the synthetic self entry.
- The same run completed four CreateAndJoin parses with `S_ROOM_ENTER` and no
  `PacketReader Error 65[0]` or `65[4]`; all CSNZ/launcher/CSOHLDS processes
  remained alive. The reported one-off crash was not reproduced or identified
  in this run.
- Removed the ineffective Lobby(153) synthetic self entry and restored
  `users=0` to preserve the login-established local host identity. Retained the
  compact room FullUserInfo (working level and clean parser) and the explicit
  family-battle reset.
- Release build succeeded and the integrated stack was restarted: server PID
  67040, launcher PID 64752, dedicated PID 50192. Host authority, level, and
  repeated mode/map changes require one more visual retest.
- Upper-left self information remains unresolved and must be implemented via
  the actual login/local-profile bootstrap packet rather than Lobby(153).

### 2026-07-17 Ghidra MCP hw.dll payload audit

- Connected to the active `hw.dll` Ghidra project through Ghidra MCP and
  cross-checked the decoders against `Packets_sampel/create_room` and
  `Packets_sampel/3`. The consolidated schemas are documented in
  `docs/hw_dll_packet_payloads.md`.
- Confirmed decoder entry points: Room 65 `FUN_0268a4c0`, Host 68
  `FUN_02664f20`, UserStart 150 `FUN_026a36b0`, Lobby 153 `FUN_02673490`,
  UserUpdateInfo 157 `FUN_026a1840`, and FullUserInfo `FUN_026bd6c0`.
- Corrected the previous Room 65 interpretation: local host authority is set
  by comparing the response's `uint32 hostUserId` with the local member user
  id. The following `uint16` is a distinct room/session slot and is not the
  host-authority field. The live capture contains host id `0x00DD43E1` and
  slot `0x00E1`.
- Confirmed the direct Host 68/101 parser defect: every inventory item ends
  with an unconditional `uint32` after its parts array, while
  `SendHostUserInventory` currently omits it. This accounts for the observed
  `PacketReader Error 68[101]`.
- Confirmed UserStart 150 field order, but the current sender zeros
  country/region/UserSN and does not provide the live Steam account id.
  UserUpdateInfo 157 is `userId + FullUserInfo`; the current zero flags carry
  no data capable of populating the upper-left self profile.
- Confirmed Host 68/0 as `uint32 + uint8 + uint8 + uint64 token` and the
  client-received 68/5 as `IPv4 + port + uint64 token`. The live token is
  dynamic and shared by the observed 68/0 and opposite-direction 68/5 flow;
  current constants/user-id substitution are not protocol-correct.
- No server implementation was changed during this audit. Next changes should
  begin with the exact 68/101 boundary fix, followed by the compact 157 self
  bootstrap and the shared Host session-token path, each with a release build
  and runtime test.

### 2026-07-17 hw.dll payload fixes implemented and built

- `SendHostUserInventory` now writes the unconditional trailing `uint32` that
  `FUN_026669b0` consumes after every item's parts array. The existing
  `CUserInventoryItem::m_nLockStatus` is used for this field, eliminating the
  known structural cause of `PacketReader Error 68[101]`.
- `SendUserUpdateInfoMinimal` no longer sends an empty 64-bit flag mask. It now
  sends the mapped compact `4000300A/00000003` FullUserInfo with the local game
  name, level, clan, tournament, and mapped trailing fields. This targets the
  missing upper-left self profile without reintroducing a synthetic Lobby 153
  member.
- Added a nonzero per-match Host session token to `CRoom`. Host 68/0 and the
  client-received dedicated-connect 68/5 now share that token. Removed the
  fixed `5555` and the previous user-id-as-token behavior.
- Corrected the dedicated Host 68/0 call to send the actual host user id. The
  preceding implementation passed the dedicated port as `userId`.
- Corrected the Room 65 source comment: host authority comes from the
  `uint32 hostUserId`; the following `uint16` is a separate, not-yet-mapped
  room/session slot. Its current compatibility value was intentionally left
  unchanged because the live slot allocator is not yet identified.
- Win32 Release build succeeded and produced `bin/Release/CSNZ_Server.exe`.
  A direct server smoke test loaded all metadata, bound TLS port 30002, and
  remained stable. A second headless server + CSOHLDS test kept both processes
  alive for 12 seconds and completed dedicated metadata registration/streaming.
- No client UI automation was used. Actual room start must now verify that the
  upper-left self profile appears, 68/101 no longer reports a reader error,
  and 68/0 plus 68/5 log the same token before dedicated game entry.

### 2026-07-17 remote sync and resource lifetime audit

- Fast-forwarded the tracked server baseline to `fork/main` commit
  `9ff8c30`. The pre-sync tracked work remains recoverable in
  `stash@{0}`; existing untracked diagnostics were left untouched.
- Added a virtual event destructor and deterministic queue cleanup. Packet
  events now retain receive packets with shared ownership, including socket
  cancellation paths.
- Closed joined/reused thread handles and added complete TCP, UDP, wolfSSL,
  client, channel-server, and dedicated-server shutdown cleanup.
- Fixed leaked SQLite script buffers and active transactions, plus packet-file
  handles and array deletion in `SendPacketFromFile`.
- Win32 Release server build and core tests succeeded (7 cases, 44 assertions).
  The current remote network test still fails its first send/receive scenario
  before teardown; this remains a separate network behavior regression.

### 2026-07-17 room host and dedicated handoff patch

- Corrected the current-client Host 68/5 payload to the server-to-client dump
  shape: endpoint string, port, IPv4, and the shared 64-bit match token.
- Added the captured client preparation order (68/114, 68/0, 68/12, 68/13)
  before the final 68/114 and 68/5 dedicated endpoint handoff.
- Room creation now follows CreateAndJoin with an explicit Room/SetHost for the
  creator. This makes host authority deterministic when the client otherwise
  presents the Ready action despite the matching host id in CreateAndJoin.
- Asset smoke testing also identified two deployment prerequisites: mount
  `HLDS_CSNZ/Docs/fixtrike.nar` under `asset/Data` and provide the bundled
  `v_dediweapon.mdl` under `asset/models`.

### 2026-07-17 room start follow-up

- Compared `Packets_sampel/create_room` with the local room flow. The sample
  folder covers Room/CreateAndJoin and Room/UpdateSettings, but it does not
  include the later Host 68 dedicated handoff packets.
- Ghidra-backed host-state analysis showed that the client decides whether to
  show Start or Ready by comparing the Room/CreateAndJoin `hostUserId` and the
  later Room/SetHost user id with the local user id. The server now avoids
  broadcasting a Ready=NO response for host ready requests and sends SetHost as
  the final role-changing packet.
- Fixed a room-setting split during game start. The client CreateAndJoin path
  already preserved the current-client raw room settings block, but the
  dedicated-server CreateAndJoin path rebuilt settings from `CRoomSettings`.
  This made the client and CSOHLDS disagree about the selected map. Both
  targets now receive the same preserved raw settings block.
- Release build succeeded after this change and the core test executable passed
  all 7 cases / 44 assertions.
- Runtime verification with asset clients confirmed that the dedicated server
  now parses the same map as the client (`#CSO_nowayout1(403)` in the observed
  run). The remaining blocker is CSOHLDS terminating with
  `Mod_NumForName: models/v_dediweapon.mdl not found` when the game starts.
- Deployed the bundled MDL under multiple asset lookup candidates:
  `asset/Bin/models`, `asset/Bin/cstrike/models`,
  `asset/Bin/cstrike-online/models`, `asset/Bin/valve/models`, and also as
  both `v_dediweapon.mdl` and `p_dediweapon.mdl`. The next room-start test
  should confirm whether this is sufficient or whether the HLDS precache path
  itself must be patched.

### 2026-07-17 room creator host-state follow-up

- User testing showed that after creating a room, the creator still saw the
  Ready button. Pressing it caused the server to re-send Room/SetHost and the
  client displayed the host-change notice.
- Rechecked `Packets_sampel/create_room`: the captured sequence is a large
  Room/CreateAndJoin, then Addon `73 00 00`, then a Room/UpdateSettings
  follow-up. The previous local flow sent Room/SetHost immediately after
  CreateAndJoin, before the client finished room UI initialization.
- Changed the server flow so `CRoom::SendJoinNewRoom` only sends
  CreateAndJoin. On the first Addon packet received from the room host, the
  server now sends Room/UpdateSettings followed by Room/SetHost, once per room
  user. This better matches the captured post-create timing and avoids repeated
  host-change messages.
- Rebuilt `bin/Release/CSNZ_Server.exe` successfully after stopping the old
  running server process that had locked the output file. Core tests passed
  again: 7 test cases / 44 assertions.

### 2026-07-17 latest-client shop connection wait fix

- Ghidra analysis of asset `hw.dll` `Packet_Shop` (`FUN_026962f0`) confirmed
  that subtype 3 replies contain a success byte and, on success, a `uint16`
  result count. The live sample `Packet_228_ID_72_5.bin` is exactly
  `48 03 01 00 00`.
- The server previously ignored inbound Shop/3, leaving the shop UI waiting
  behind the `CSO_Trying_To_Connect_A_Server` popup. It now returns the exact
  successful empty-result response.
- `Packet_UpdateInfo` analysis showed server-to-client cases 2, 3, 7, 11, and
  16 only. Client notifications 69/9 and 69/12 are therefore not answered with
  invented payloads; 69/12 remains the known inventory-button notification.
- Runtime verification observed RX `48 03` immediately followed by TX
  `48 03 01 00 00`. The client stayed connected and proceeded through room
  creation and game-start requests.
- Also stopped the accidental ClientCheck loop: ID 66 from the client is the
  report generated after the one server challenge, not a request for another
  challenge. The new run sends one challenge and receives one report.
- A separate blocker remains after handoff: CSOHLDS disconnected roughly two
  seconds after the game-start sequence. This is independent of the resolved
  shop response wait and is the next dedicated-server diagnostic target.

### 2026-07-17 local identity and room-host regression

- After the remote sync, `SendLobbyJoin` inserted a local Lobby(153) member
  without the current SUID. That incomplete identity could be treated as a
  remote member, producing Ready instead of Start. An empty Lobby snapshot
  preserved host state but left menus blocked before they emitted requests.
- Ghidra `FUN_026bd6c0` shows low FullUserInfo bit 27 is a four-byte session
  user/SUID field. The compact profile omitted it, so the UI could not reliably
  associate nickname/level data with the login-established local user.
- Added bit 27 and the current user id to the compact FullUserInfo used by
  UserUpdateInfo(157), room users, and lobby joins. The resulting login packet
  is 78 bytes and parses without `PacketReader Error 157[1]`. Lobby(153) now
  includes one self entry carrying the same UID/SUID pair.
- A diagnostic attempt to reuse the older 471-byte Steam full-profile sample
  was rejected: current asset `hw.dll` consumed 151 additional bytes and
  overread the packet. That experiment was removed rather than padding an
  obsolete schema.
- Release target `PROJECTNAME` builds successfully. Integrated runtime reached
  login, metadata, Lobby(153), room creation, `S_ROOM_ENTER(1)`, shop requests
  72/15 and 72/16, successful Shop/3, game start, and dedicated assignment.
  This proves the former menu popup now advances far enough to send and receive
  its server requests. The next blocker was a dedicated model file and is
  tracked in `HLDS_CSNZ/PROGRESS.md`.
# 2026-07-17 latest UpdateInfo(69/9) loading-modal fix

- The latest runtime reached login and lobby initialization, but opening the
  shop/inventory path sent `UpdateInfo(69/9)` with state `0` and `1`. The
  server discarded both as `unknown request 9`, leaving the client's
  `서버에 연결 중입니다` modal active.
- `Packets_sampel/create_room` shows the older equivalent transition as
  `69/7 00 01`, immediately followed by `UserUpdateInfo(157)`. The asset
  client has moved that request subtype from 7 to 9.
- Added `RequestUpdateTutorialLatest = 9` and made both subtype 7 and subtype
  9 refresh the current compact `UserUpdateInfo(157)` profile.
- Release compilation/link verification succeeded as
  `bin/Release/CSNZ_Server_next.exe` (2,619,904 bytes, built 19:01:56).
  The normal output remained locked by the administrator-owned running
  `CSNZ_Server.exe`; stop that process, replace/rebuild the normal executable,
  and confirm that logs contain `RequestTutorial: type=9 ... refreshing` and
  a following `TX UserUpdateInfo(157)`.
