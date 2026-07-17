# hw.dll 클라이언트 수신 패킷 페이로드 분석

분석일: 2026-07-17

이 문서는 현재 Ghidra 프로젝트의 `hw.dll` 디코더와
`Packets_sampel/create_room`, `Packets_sampel/3`의 실서버 Steam 패킷을
교차검증한 결과다. 아래 구조에서 패킷 ID 바이트는 제외하며, 정수는 별도
표기가 없으면 little-endian이다. `string`은 게임 패킷 리더가 읽는 NUL 종료
문자열이다.

## 결론 요약

| ID | 패킷/서브타입 | 클라이언트가 기대하는 핵심 페이로드 | 현재 구현 상태 |
|---:|---|---|---|
| 65 | Room / 0 | 방 정보, `u32 hostUserId`, 방 설정, 멤버 목록 | 큰 틀은 일치. `u16 roomSlot`을 user ID로 취급한 주석/구현은 잘못됨 |
| 68 | Host / 0 | `u32 userId + u8 category + u8 flags + u64 sessionToken` | 방 단위 동적 토큰으로 수정, 런타임 방 시작 확인 필요 |
| 68 | Host / 5 | `string endpoint + u16 port + u32 IPv4 + u64 sessionToken` | 실서버 수신 덤프와 같은 endpoint 형식으로 수정 |
| 68 | Host / 101 | 소유자, 아이템 배열, 아이템마다 마지막 `u32` | `m_nLockStatus`를 마지막 `u32`로 추가, 런타임 요청 확인 필요 |
| 150 | UserStart | 로컬 계정/캐릭터/지역/UserSN 초기화 | 순서는 일치하지만 계정 식별자와 지역/UserSN 값이 불완전 |
| 153 | Lobby | 로비 사용자 목록 증감 | 자기 프로필 초기화 용도가 아님 |
| 157 | UserUpdateInfo | `u32 userId + FullUserInfo` | compact self FullUserInfo를 보내도록 수정, UI 확인 필요 |

가장 직접적인 파서 오류는 Host 68/101의 아이템별 마지막 `u32` 누락이다.
좌측 상단 자기 정보는 Lobby 153에 synthetic self를 넣는 방식이 아니라,
UserStart 150 뒤에 유효한 FullUserInfo를 가진 UserUpdateInfo 157을 보내는 흐름이
Ghidra 소비 코드와 일치한다. 다만 UI 표시 성공 여부는 구현 후 런타임 검증이
필요하다.

## 65: Packet_Room, subtype 0 (CreateAndJoin)

디코더: `FUN_0268a4c0`

소비/방장 상태 설정: `FUN_026259f0`

방 설정 디코더: `FUN_0268ecf0`

멤버 prefix 디코더: `FUN_0268d5f0`

```text
u8    subtype                 // 0
u8    result
u32   roomId
u32   hostUserId
u16   roomSlot
u8    state                  // 실서버 캡처: 0x05
RoomSettings settings
u8    memberCount
repeat memberCount {
    u32    userId
    u32    unknown
    string accountId
    u8     team
    u8     state1
    u8     state2
    u8     state3
    u32    externalIPv4
    u16    externalClientPort
    u16    externalServerPort
    u32    localIPv4
    u16    localClientPort
    u16    localServerPort
    FullUserInfo userInfo
}
```

`Packets_sampel/create_room/Packet_199_ID_65_841.bin`은 헤더가 다음과
같이 시작한다.

```text
41 00 00 1F000000 E143DD00 E100 05 ...
```

- `hostUserId = 0x00DD43E1`
- `roomSlot = 0x00E1`
- 같은 세션의 UserStart 150의 `userId`도 `0x00DD43E1`

`FUN_026259f0`는 `u32 hostUserId`를 현재 로컬 멤버의 user ID와 비교해
방장 상태를 설정한다. `u16 roomSlot`은 방장 권한 판정 필드가 아니다. 따라서
현재 소스의 `WriteUInt32(hostId)`는 권한 판정에 필요한 필드이고, 뒤의
`WriteUInt16(hostId)`는 프로토콜 의미상 별도의 세션 room slot 값으로
모델링해야 한다.

현재 sparse RoomSettings mask에서 확인된 순서는 다음과 같다.

```text
u32 lowMask
u32 lowMidMask
u32 highMidMask
u32 highMask

low bit 0     -> string roomName
low bit 3     -> string password
low bit 6     -> u8 gameModeId
low bit 7     -> u16 mapId
low bit 8     -> u8 maxPlayers
low bit 9     -> u8 winLimit
low bit 10    -> u16 killLimit
lowMid bit 8  -> u8
lowMid bit 13 -> u16 + u32 + u32
```

mask bit 중에는 payload가 없는 marker도 있으므로, 설정된 모든 bit가 필드를
하나씩 소비한다고 가정하면 안 된다.

## FullUserInfo

디코더: `FUN_026bd6c0`

```text
u32 lowFlags
u32 highFlags
conditional fields...
```

현재 compact flags `low=0x4000300A`, `high=0x00000003`에서 확인된 필드는
다음과 같다.

```text
low bit 1   -> string + string + string
low bit 3   -> u32 level
low bit 12  -> u32 clanId + u32 clanMarkId + string clanName
               + u8 + u8 + u8
low bit 13  -> u8 tournament/packed value
low bit 30  -> u8 + u8
high bit 0  -> u16
high bit 1  -> marker only; payload 없음
```

`WriteLatestRoomFullUserInfo`의 compact serializer는 이 구조와 일치한다.

## 150: Packet_UserStart

디코더: `FUN_026a36b0`

```text
u32    userId
string accountId
string characterName
u8     firstConnect
u8     countryCode
u8     regionCode
u32    userSN
u8     unknown0
u8     unknown1
```

`Packets_sampel/3/Packet_3_ID_150_41.bin`의 값은 다음과 같다.

```text
accountId     = "76561198674154653"
characterName = "cp121503"
firstConnect  = 1
countryCode   = 9
regionCode    = 1
userSN        = 0x006478D6
unknown0/1    = 0/0
```

현재 `SendUserStart`는 필드 순서는 맞지만 `accountId`에 서버 username을 쓰고,
country/region/UserSN을 전부 0으로 쓴다. Ghidra 소비 코드는 이 값을 로컬
계정/캐릭터 상태에 보관하므로 실제 로그인 식별자와 메타데이터를 공급해야
한다.

## 153: Packet_Lobby

디코더: `FUN_02673490`

```text
subtype 0:
    u16 count
    repeat count {
        u32 userId
        string accountId
        FullUserInfo userInfo
    }

subtype 1:
    u32 userId
    string accountId
    FullUserInfo userInfo

subtype 2:
    u32 userId
```

이 패킷은 로비 roster를 갱신한다. subtype 0에 synthetic self를 넣었을 때
좌측 상단 자기 정보가 생기지 않았고 방장 동작이 Ready-only로 회귀한 런타임
결과도 이 용도 구분과 일치한다.

## 157: Packet_UserUpdateInfo

디코더: `FUN_026a1840`

소비/모델 갱신: `FUN_026bcb60`

```text
u32 userId
FullUserInfo userInfo
```

실서버의 471-byte 패킷은 `lowFlags=0xFEDFFFFF`,
`highFlags=0x00000005`를 사용한다. 작은 캡처도 bit 3이 `u32`를 소비하고,
high bit 1은 payload 없는 marker임을 확인해 준다.

현재 `SendUserUpdateInfoMinimal`은 `u64 flags=0`만 보내므로 유저 모델/UI를
갱신할 필드가 전혀 없다. 우선 이미 정확히 매핑된 compact flags
`0x4000300A/0x00000003`와 동적 자기 정보를 UserStart 150 직후 157로 보내는
것이 가장 좁고 검증 가능한 수정 후보이다.

## 68: Packet_Host

주 디코더: `FUN_02664f20`

### subtype 0 (GameStart/host state)

```text
u32 userId
u8  serverCategory
u8  flags
u64 sessionToken
```

`Packets_sampel/3/Packet_208_ID_68_16.bin`은 이 구조와 정확히 일치하며
`sessionToken=0x01106A58F52C1E4F`이다. 현재 `SendHostGameStart`의 폭과 순서는
맞지만 토큰을 `5555`로 고정하므로 실서버의 세션 연계 방식과 다르다.

### subtype 5 (클라이언트 수신 dedicated connect)

```text
string endpoint
u16    port
u32    IPv4
u64    sessionToken
```

`Packets_sampel`은 `Hook_HWSocket_ReadPacket`이 저장한 서버→클라이언트
수신 덤프다. 따라서 `Packet_212_ID_68_40.bin`의 문자열형 endpoint가 현재
클라이언트가 실제로 소비하는 형식이다. 뒤의 IPv4는 endpoint가 해석된 주소와
같고, 마지막 token은 앞선 subtype 0과 동일하다.

### subtype 101 (SetInventory)

배열 디코더: `FUN_026669b0`

```text
u32 ownerUserId
u16 itemCount
repeat itemCount {
    u16 itemId
    u8  state
    u16 count
    u16 paintId
    u16 enhancementLevel
    u32 enhanceValue
    u8  partCount
    repeat partCount {
        u8  slot
        u16 partId
    }
    u32 trailingValue
}
```

마지막 `u32 trailingValue`는 part 배열 뒤에 아이템마다 조건 없이 읽힌다.
현재 `SendHostUserInventory`에는 이 필드가 없어 다음 아이템 또는 패킷 끝을
읽게 되며, 이는 런타임의 `PacketReader Error 68[101]`과 정확히 일치한다.
의미는 아직 확인되지 않았지만 폭과 위치는 확정이다. `Packets_sampel/3`에는
subtype 101 실서버 표본이 없으므로 의미를 lock/expiry 등으로 단정하지 않는다.

## 수정 우선순위

1. Host 68/101 아이템마다 마지막 `u32`를 추가하고 초기값 0으로 파서 경계를 검증한다.
2. 로그인 시 UserStart 150에 실제 account/지역/UserSN을 공급한다.
3. UserStart 직후 compact FullUserInfo를 가진 UserUpdateInfo 157을 보내 좌측 상단 자기 정보를 검증한다.
4. Room 65의 `u16 roomSlot`을 user ID와 분리해 세션별 값으로 관리한다.
5. Host 68/0과 68/5가 동일한 동적 `u64 sessionToken`을 공유하도록 토큰 생성/전달 경로를 구현한다.

68/101의 경계 수정은 Ghidra로 확정되었다. 157 compact profile과 68의 토큰
수정은 디코더 구조상 강하게 뒷받침되지만, 실제 UI 표시와 dedicated 접속은
각각 런타임 재검증이 필요하다.
