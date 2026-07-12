# CSNZ_Server Progress

Last updated: 2026-07-12

## Current Goal

Adapt the server login/bootstrap protocol to the latest CSNZ `hw.dll` so the launcher can authenticate locally and proceed to the lobby without crashing or disconnecting.

## Metadata Source Rule

`bin/LiveMetadata` is the authoritative metadata source for local client tests.
These files were captured from the live server and must not be regenerated from
`Data` or `MetadataArtifacts` during protocol work. Fallback metadata remains
available for older diagnostics, but the default latest-client stream should only
send payloads that are backed by `LiveMetadata` or explicitly isolated with
`CSNZ_METADATA_FILTER`.

## Implemented

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
- live server packets are stored on `Packets_sample` directory. It can be used to analyze how client-server connect
