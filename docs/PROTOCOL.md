# vplayer 제어 프로토콜 명세 (v1)

- 문서 버전: v1.0 (2026-07-03)
- 대상: C++/GStreamer 기반 `vplayer.exe` — 기존 Python/VLC 플레이어(`player_python/player.py`)의 **드롭인 교체** 구현체
- 근거 소스 (ground truth):
  - `vp_app2/player_python/player.py` — 현행 플레이어 (명령 디스패치, 피드백 송신)
  - `vp_app2/src/player/index.js` — 호스트 spawn / 포트 핸드셰이크 / NDJSON 프레이밍 / `playerSend`
  - `vp_app2/src/player/parser.js` — 호스트 피드백 소비자
  - `vp_app2/src/api/player/index.js`, `vp_app2/src/api/playlists/index.js` — 호스트가 실제로 보내는 명령 JSON 형태

이 문서는 **구현 계약(contract)** 이다. vplayer는 여기 기술된 메시지 형태와 타이밍을 그대로 재현해야 하며, 호스트(vp_app2) 코드는 수정하지 않는 것을 전제로 한다.

---

## 1. 전송 계층 (Transport)

### 1.1 프로세스 기동

호스트(Electron 메인 프로세스)가 플레이어를 자식 프로세스로 spawn 한다 (`src/player/index.js`).

- `stdio: ['pipe', 'pipe', 'pipe']`, `shell: false`
- 환경 변수 (현행 Python 기준, vplayer도 동일하게 받음):

| 환경변수 | 내용 |
|---|---|
| `VP_PSTATUS` | 호스트 `pStatus` 객체 전체를 `JSON.stringify` 한 문자열 |
| `APP_PATH` | 앱 루트 경로 (윈도우 아이콘 `src/icon.png` 탐색용) |
| `PYTHONUNBUFFERED=1`, `PYTHONIOENCODING=utf-8` | Python 전용. vplayer는 무시해도 되나 **stdout은 라인 단위 즉시 flush** 해야 함 |

**`VP_PSTATUS` 로 전달되는 초기 상태** — 호스트 `pStatus` 객체의 실제 키(`src/pStatus.js`):

```json
{
  "windowOpen": false,
  "playlistMode": false,
  "playlist": {},
  "trackId": 0,
  "repeat": "none",
  "startOnPlay": false,
  "startOnPlaylistId": null,
  "audioDevices": [],
  "audioDevice": "",
  "logoFile": "",
  "logoShow": true,
  "logoSize": 0,
  "file": {},
  "player": { "event": "", "volume": 100, "speed": 1.0, "position": 0, "duration": 0, "time": 0 },
  "activePlayerId": 0,
  "fullscreen": false,
  "backgroundColor": "#000000"
}
```

플레이어가 초기값으로 실제 사용하는 키: `playlistMode`(bool), `fullscreen`(bool), `audioDevice`(string).

> **주의 (현행 불일치):** Python 플레이어는 `pstatus.get("playlistTrackIndex")`, `pstatus.get("logo", {}).get("file"/"show"/"size")`, `pstatus.get("background")` 를 읽지만, 호스트 `pStatus`에는 이 키들이 **존재하지 않는다** (`logoFile`/`logoShow`/`logoSize`/`backgroundColor` 라는 평탄한 키로 존재). 따라서 로고·배경·트랙 인덱스의 실질적 초기화는 항상 **ready 이후 호스트가 보내는 명령**(§1.5)으로 이루어진다. vplayer는 `VP_PSTATUS`를 "힌트"로만 취급하고, `playlistMode` / `fullscreen` / `audioDevice` 만 초기 적용하면 충분하다.

### 1.2 TCP 서버 바인드 + stdout 포트 핸드셰이크

1. 플레이어는 기동 직후 **`127.0.0.1:1300`** 에 TCP 서버를 바인드한다 (`SO_REUSEADDR`, backlog 1). 1300 바인드 실패 시 대체 포트(예: 포트 0 → OS 임의 할당)로 바인드한다 — 호스트는 stdout으로 보고된 포트를 그대로 사용하므로 포트 번호 자체는 자유다.
2. 바인드 성공 후 **stdout에 단 한 줄** 을 출력하고 flush 한다:

```json
{"type":"port","data":{"port":1300}}
```

3. **이후 stdout은 절대 사용하지 않는다.** 모든 피드백은 TCP로만 보낸다. (호스트 `src/player/index.js`는 stdout 각 라인을 `JSON.parse` 하며, `type === 'port'` 라인에서 TCP 접속을 개시하고 그 외 라인은 parser로 넘긴다. JSON이 아닌 라인이 섞이면 해당 stdout 청크 전체 처리가 `catch`로 빠져 **포트 라인까지 유실될 수 있다**.)
4. stderr는 호스트가 그대로 `logger.error` 로 기록한다. 크래시 덤프 용도로만 사용하고 정상 동작 중에는 출력하지 않는 것을 권장.

### 1.3 NDJSON 프레이밍

- 호스트가 TCP **클라이언트**로 접속한다 (`createConnection({host:'127.0.0.1', port})`).
- 양방향 모두 **개행(`\n`)으로 구분된 JSON 한 줄 = 메시지 하나** (NDJSON). 인코딩 UTF-8.
- 플레이어 송신: `json.dumps(..., ensure_ascii=False, separators=(",",":")) + '\n'` — 비ASCII(한글) 문자를 이스케이프하지 않고 그대로 보낸다. vplayer도 유효한 UTF-8 JSON 한 줄이면 형식(공백 등)은 자유.
- 호스트 송신(`playerSend`): 명령 객체에서 **`undefined` 값 키를 삭제**한 뒤 `JSON.stringify(command) + '\n'` 으로 보낸다. 즉 **옵션 필드는 "키 자체가 없음"으로 도착**한다 (`null`은 삭제되지 않고 `null`로 도착 — 예: `play_current_and_load_next`의 `"next": null`).
- 수신 측은 양쪽 모두 버퍼에 누적 후 `\n` 단위로 분해하며, 빈 라인은 무시한다. 하나의 TCP 청크에 여러 라인, 또는 한 라인이 여러 청크로 쪼개져 올 수 있음을 전제해야 한다.

### 1.4 연결 수명 / 재접속

- 소켓이 끊기면 호스트는 (플레이어 프로세스가 살아 있는 한) **1초 후 재접속**을 시도한다 (`src/player/index.js` `socket.on('close')` → `setTimeout(connectToPlayer, 1000)`). 따라서 플레이어는 클라이언트 연결 종료 후에도 서버 소켓을 유지하고 **accept 루프를 계속 돌아 재연결을 받아야 한다** (동시 클라이언트는 1개).
- 클라이언트가 연결되어 있지 않은 동안 플레이어가 보내는 메시지는 **조용히 버려진다** (현행 동작). 큐잉하지 않는다.
- **플레이어 프로세스가 종료되면 호스트도 종료된다** — 호스트는 자식 프로세스 `close` 이벤트에서 `app.quit()` 을 호출한다. 즉 플레이어 창을 닫는 것 = 앱 전체 종료. vplayer도 창 닫힘 시 프로세스를 종료하면 된다 (Python은 종료 직전 `{"type":"info","data":"Closing player window, terminating process."}` 를 보낸 뒤 exit).
- 호스트 종료 시에는 `player.kill()` 로 플레이어를 죽인다. 별도의 종료 명령은 없다.

### 1.5 초기화 시퀀스 (ready 이후)

```
호스트                                    플레이어
  │ spawn ─────────────────────────────────▶ 기동, 1300 바인드
  │ ◀───────────── stdout: {"type":"port","data":{"port":1300}}
  │ TCP connect ───────────────────────────▶ accept
  │ ◀───────────── {"type":"info","data":"Player ready"}      (TCP)
  │ {"command":"background_color","color":"#000000"} ────────▶
  │ {"command":"set_fullscreen","value":true}   (fullscreen일 때만) ─▶
  │ {"command":"get_audio_devices"} ──────────────────────────▶
  │ {"command":"playlist_mode","value":true}  (playlistMode일 때만) ─▶
  │ {"command":"logo_file","file":"C:\\...\\logo.svg"} ───────▶
  │ {"command":"logo_size","size":400} ───────────────────────▶
  │ {"command":"show_logo","show":true} ──────────────────────▶
  │ ◀───────────── {"type":"audiodevices","data":{"devices":[...]}}
  │ {"command":"set_audio_device","device_id":"..."} (설정돼 있을 때만) ─▶
```

- 호스트는 `info` 피드백 문자열에 `'ready'` 가 포함된 것을 보고 위 초기화 명령 묶음을 보낸다 (§4-①).
- Python은 기동 500ms 후에 `"Player ready"` 를 보낸다. 이 시점에 호스트 TCP 접속이 아직 안 됐으면 메시지가 유실되어 초기화가 영원히 안 되는 **경쟁 조건**이 현행에 존재한다. **vplayer는 첫 TCP 클라이언트 접속이 성립한 뒤에 ready를 보내야 한다** (접속 직후 송신 권장 — 안전한 상위호환).
- `pStatus.startOnPlay && startOnPlaylistId` 이면 호스트가 TCP 접속 1초 후 `set_tracks` + `play_current_and_load_next` 를 보내 자동 재생을 시작한다.

### 1.6 듀얼 덱(deck) 모델

플레이어는 내부에 **2개의 재생 슬롯(덱) `0`/`1`** 을 가진다 (`players[2]`, `player_widgets[2]`, `current_files[2]`).

- `active_player_id` (0 또는 1): 현재 화면/소리에 노출되는 덱.
- 나머지 덱은 standby — 플레이리스트 모드에서 다음 트랙을 **미리 로드**해 두는 자리.
- 갭리스 전환: `next` 명령이 오면 standby 덱을 즉시 기동하고 두 덱을 스왑하며, 스왑 시마다 `active_player_id` 가 0↔1 교대된다 (§4-②의 전제).

---

## 2. 명령 (호스트 → 플레이어)

모든 명령은 `{"command": "<이름>", ...필드}` 형태의 JSON 한 줄이다. 알 수 없는 명령 수신 시 `{"type":"error","data":"Unknown command: <이름>"}` 를 보내고 무시한다(크래시 금지). 명령 실행 중 예외 발생 시 `{"type":"error","data":"Error executing '<이름>': <사유>"}`.

### 2.0 명령 요약표

| command | 필드 | 호스트 발신 위치 | 비고 |
|---|---|---|---|
| `show_logo` | `show`: bool | api/player `showLogo`, ready 초기화 | 로고 표시/숨김 |
| `logo_file` | `file`: string(절대경로) | api/player `setLogoFile`, ready 초기화 | 로고 파일 교체 |
| `logo_size` | `size`: int(px) | api/player `setLogoSize`, ready 초기화 | 로고 폭 지정 |
| `set_media` | `file`: obj, `idx`: 0\|1 | api/player `setMedia` | 덱에 미디어 로드(재생 안 함) |
| `playid` | `file`: obj | api/player `playId`/`playFile` | 파일 즉시 재생(덱 자동 선택) |
| `play` | `idx`: 0\|1 | api/player `play`, parser repeat_one | 지정 덱 재생 |
| `pause` | `idx`: 0\|1 | api/player `pause` | 일시정지/재개 토글 |
| `stop` | `idx`: 0\|1 (생략 가능) | api/player `stop`, parser | 덱 정지 (생략 시 전체) |
| `stop_all` | 없음 | api/player `stop`, parser | 전체 정지 |
| `set_audio_device` | `device_id`: string | api/player, parser(audiodevices 후) | 출력 장치 변경 |
| `get_audio_devices` | 없음 | ready 초기화 | 장치 목록 요청 |
| `playlist_mode` | `value`: bool | api/playlists `setPlaylistMode` | 플레이리스트 모드 on/off |
| `playlist_play` | `idx`: int | **현재 호스트 미사용(레거시)** | 내부 tracks로 재생 |
| `set_tracks` | `tracks`: obj[] | api/playlists (재생 시작·편집 시) | 트랙 목록 전달 |
| `play_current_and_load_next` | `current`, `next`, `track_idx`, `current_time?`, `next_time?` | api/playlists `playlistPlay` 등 | 현재 재생 + 다음 프리로드 |
| `preload_next` | `next`, `next_track_idx`, `next_time?` | api/playlists `preloadNextTrack` | standby 덱에 다음 로드만 |
| `set_track_index` | `index`: int | **현재 호스트 미사용(레거시)** | 트랙 인덱스 강제 설정 |
| `next` | 없음 | parser `handleEndReached` | standby 덱으로 스왑 재생 |
| `previous` | 없음 | **현재 호스트 미사용(레거시)** | 이전 트랙 |
| `set_time` | `time`: int(ms), `idx`: 0\|1 | api/player `updateTime` | 재생 위치 이동 |
| `set_fullscreen` | `value`: bool | api/player, ready 초기화 | 전체화면 |
| `background_color` | `color`: string | api/player, ready 초기화 | 배경색 |

"레거시" 표기 명령도 디스패치 테이블에 존재하므로 **vplayer는 전부 구현**해야 한다 (외부 TCP/API 경유로 유입될 수 있음).

---

### 2.1 `file` 객체 (트랙 객체)

`set_media`/`playid`/`set_tracks`/`play_current_and_load_next`/`preload_next` 의 파일 인자는 호스트 DB(`dbFiles`) 레코드가 **그대로** 전달된 것이다. 플레이어가 실제 소비하는 필드:

| 필드 | 타입 | 필수 | 플레이어에서의 용도 |
|---|---|---|---|
| `path` | string | **필수** | 재생할 파일의 절대 경로 (Windows 경로, 한글 포함 가능) |
| `is_image` | bool | **필수** | 이미지/동영상 분기. **없으면 문맥에 따라 `true`(set_media/play/stop) 또는 비-이미지 아님(playid는 `is_image === false` 일 때만 디코더 재생)으로 해석되는 모순이 있으므로 항상 명시된 값으로 취급** |
| `uuid` | string | 필수(사실상) | `media_changed` 피드백에 그대로 반사 → 호스트가 DB 조회 키로 사용 |
| `mimetype` | string | 필수(사실상) | `"audio/"` 접두 여부로 오디오 파일 판정(§2.7 로고 규칙). `"video/*"`, `"image/*"` |
| `time` | number(초) | 옵션 | 이미지 표시 시간(초). `0` = 무한 표시. 없으면 기본 5초 |
| `filename` | string | 옵션 | 로그 출력용 |

그 외 필드(`id`, `number`, `originalname`, `amx`, `size`, `metadata`, `thumbnail`, `_id`, `reserved`, `updatedAt` 등)도 함께 도착하지만 플레이어는 무시한다. vplayer는 모르는 필드를 에러 없이 통과시켜야 한다.

```json
{
  "uuid": "3f2a9c1e-8b4d-4e0f-9a17-c5d2e6b81a90",
  "number": 12,
  "id": "12",
  "filename": "opening.mp4",
  "mimetype": "video/mp4",
  "size": 104857600,
  "path": "C:\\vp\\media\\3f2a9c1e-...\\opening.mp4",
  "is_image": false,
  "thumbnail": "C:\\vp\\media\\3f2a9c1e-...\\thumb.jpg",
  "time": 0
}
```

---

### 2.2 로고 명령

#### `show_logo`
```json
{"command":"show_logo","show":true}
```
- `show`: bool (누락 시 `true`).
- 동작: 로고 위젯 표시/숨김. 표시 시 최상위로 raise, 창 중앙 정렬.
- 피드백: `{"type":"logo_visibility","data":{"show":true}}` + `debug`.

#### `logo_file`
```json
{"command":"logo_file","file":"C:\\vp\\logo\\logo.svg"}
```
- `file`: 절대 경로 문자열. 확장자 `.svg`(대소문자 무관)면 SVG 렌더, 그 외는 비트맵.
- 파일이 없거나 빈 문자열이면 `{"type":"warn","data":"Logo file not found: <path>"}` 만 보내고 무시.
- 성공 시 기존 `logo_size` 값으로 크기 재계산·재배치. 전용 ack 피드백 없음(`debug` 만).

#### `logo_size`
```json
{"command":"logo_size","size":400}
```
- `size`: int, **로고의 목표 폭(px)**. 높이는 원본 비율 유지로 자동 계산. `size <= 0` 이면 원본 크기 사용.
- 로고 파일이 미설정/부재면 `error` 피드백 후 무시. 전용 ack 없음(`debug` 만).

---

### 2.3 기본 재생 명령

#### `set_media` — 덱에 미디어 로드 (재생 시작 안 함)
```json
{"command":"set_media","file":{ "...": "file 객체" },"idx":0}
```
- `idx`: 0 또는 1 (호스트는 `pStatus.activePlayerId || 0` 을 보냄). 범위 밖이면 `error`.
- 동작:
  - `current_files[idx] = file` 저장.
  - `file.is_image` 가 참(또는 **누락**)이면: 즉시 이미지를 해당 덱 위젯에 표시(`display_image`) — 주의: set_media인데도 이미지는 바로 그려진다.
  - 비디오/오디오면: 디코더 파이프라인에 로드만 하고 재생하지 않는다. 현재 로드된 미디어와 경로가 같으면 재로드 생략.
- 피드백: **`idx == active_player_id` 인 경우에만** `media_changed` 발신 (§3.4). standby 덱 프리로드 시에는 절대 발신하지 않는다. 이미지인 경우 추가로 `player_data`(event `display_image`, §3.3) 1회.

#### `playid` — 파일 즉시 재생 (덱 자동 선택)
```json
{"command":"playid","file":{ "...": "file 객체" }}
```
- 덱 선택 규칙: 현재 `active_player_id` 덱이 사용 중(재생 중이거나 이미지 표시 중)이면 **반대 덱**을 선택한다.
- 순서(피드백 순서 계약): ① `active_player_id` 피드백(새 덱) → ② `set_media` 수행(→ `media_changed`, 이미지면 `player_data(display_image)`) → ③ `is_image === false` 이면 재생 시작 → ④ 전환(fade_transition): 이전 덱 조용히 정지(피드백 없음), 로고 처리(§2.7) → `logo_visibility` 피드백 → `active_player_id` 피드백(동일 값 한 번 더 — 중복 발신이 현행 동작).
- 이후 100ms 마다 `player_data` 틱 (§3.3).

#### `play` — 지정 덱 재생
```json
{"command":"play","idx":0}
```
- `idx`: int (누락 시 0). 호스트는 `pStatus.activePlayerId || 0`.
- `current_files[idx].is_image` 가 참(또는 누락)이면 이미지 재표시, 아니면 디코더 재생(일시정지 상태였다면 이어서 재생). 이어서 fade_transition(idx) 실행 → `logo_visibility`, `active_player_id` 피드백.
- parser의 playlist `repeat_one` 처리에서 `stop`+`play` 조합으로 같은 덱 재재생에 사용된다.

#### `pause` — 일시정지 토글
```json
{"command":"pause","idx":0}
```
- **현행 quirk:** `idx` 는 범위 검증에만 쓰이고, 실제로는 항상 `active_player_id` 덱을 pause 한다. VLC `pause()` 는 토글(일시정지↔재개)이다. vplayer도 동일하게: 활성 덱 토글.
- 피드백 없음. (pause 중에는 `is_playing` 이 false가 되어 `player_data` 틱이 멈춘다 — 호스트 UI는 이를 근거로 정지 상태를 안다.)

#### `stop` / `stop_all`
```json
{"command":"stop","idx":1}
{"command":"stop_all"}
```
- `stop` 에서 `idx` 누락(`data.get("idx")` → None) 시, 그리고 `stop_all` 은 **모든 덱 정지**.
- 전체 정지 시: 이미지 타이머 정지(`debug` "Image timer stopped"), 각 덱에 대해 이미지면 `stop_image`(→ `player_data` event `stop_image`), 비디오면 조용히 stop, 위젯 숨김. `{"type":"info","data":"All players stopped"}`.
- 특정 덱 정지 시: 해당 덱만 위와 동일 처리(info 없음).
- 공통 마무리: 로고 표시로 전환 → `{"type":"logo_visibility","data":{"show":true}}`.

#### `set_time` — 재생 위치 이동
```json
{"command":"set_time","time":15000,"idx":0}
```
- `time`: **밀리초(ms)**, 정수, `>= 0`. 음수/비정수는 `error` 후 무시.
- `idx`: 대상 덱 (누락 시 0 — 디스패치 기본값 때문에 "활성 덱" 폴백 분기는 실제로 도달 불가). 호스트는 `pStatus.activePlayerId || 0`.
- 이미지에는 효과 없음. 피드백 없음(다음 `player_data` 틱에 반영된 time으로 확인).

---

### 2.4 오디오 디바이스

#### `set_audio_device`
```json
{"command":"set_audio_device","device_id":"{0.0.0.00000000}.{guid...}"}
```
- `device_id`: `get_audio_devices` 가 보고한 `deviceId` 값. **빈 문자열이면 시스템 기본 장치**로 설정.
- **두 덱 모두**에 적용한다. 성공 시 `debug`, 실패 시 `error` — 전용 ack 피드백은 없다.
- ready 직후 흐름: 호스트는 `audiodevices` 피드백을 받은 뒤에야 저장된 장치로 이 명령을 보낸다 (parser.js `case 'audiodevices'`).

#### `get_audio_devices`
```json
{"command":"get_audio_devices"}
```
- 피드백: `{"type":"audiodevices","data":{"devices":[{"deviceId":"...","name":"..."}]}}` (§3.5).
- 장치 열거 실패 시 `error`.

---

### 2.5 플레이리스트 명령

플레이리스트의 **진행/반복 판단은 전적으로 호스트 책임**이다 (§4-④). 플레이어는 (a) 현재 트랙 재생, (b) 다음 트랙 프리로드, (c) `next` 수신 시 덱 스왑, (d) 끝났을 때 `end_reached` 보고만 한다.

#### `playlist_mode`
```json
{"command":"playlist_mode","value":true}
```
- 내부 `playlist_mode` 플래그 설정. `debug` 만 발신. 이 플래그가 참일 때만 `media_changed` 에 `playlist_track_index` 가 포함되고, 이미지 타이머·`next`/`previous` 가 동작한다.

#### `set_tracks`
```json
{"command":"set_tracks","tracks":[{ "...": "file 객체" }, { "...": "..." }]}
```
- 트랙(=file 객체) 배열 전체를 저장만 한다. 피드백 없음. 배열이 아니면 `error`.
- 호스트는 플레이리스트 재생 시작 시(`playlistPlay`)와 재생 중 플레이리스트 편집 시마다 전체 목록을 다시 보낸다. 현행 플레이어에서 이 목록은 레거시 경로(`playlist_play`/`previous`)와 `set_track_index`·이미지 타이머의 범위 검증에만 쓰인다.

#### `play_current_and_load_next` — 주 재생 진입점
```json
{
  "command": "play_current_and_load_next",
  "current": { "...": "file 객체" },
  "next":    { "...": "file 객체" },
  "track_idx": 0,
  "current_time": 5,
  "next_time": 10
}
```
- `current`: 지금 재생할 트랙(file 객체). 없으면 `error`.
- `next`: 프리로드할 다음 트랙. **마지막 트랙이면 `null` 로 도착**한다 (undefined가 아니라 null — `playerSend` 는 undefined만 삭제).
- `track_idx`: `current` 의 플레이리스트 내 인덱스 (0-base).
- `current_time` / `next_time`: **초 단위** 이미지 표시 시간. 해당 트랙이 이미지일 때만 존재하고(비이미지는 undefined → **키 자체가 삭제되어 미도착**), 호스트가 `track.time || 5` 로 계산하므로 0은 오지 않는다(0 → 5초).
- 동작 순서:
  1. `track_index = track_idx` 반영 → `{"type":"track_index","data":{"value":track_idx}}` 발신.
  2. `current.is_image` 이고 `current_time` 존재 시 `current.time = current_time` 으로 덮어씀. `info` "Playing track ..." 발신.
  3. `playid` 와 동일한 경로로 `current` 재생 (덱 선택·`active_player_id`·`media_changed`·`logo_visibility` 발신 포함).
  4. `next` 가 null이 아니면: `next_time` 병합 후 **standby 덱**(활성 반대 덱)에 `set_media` — `media_changed` 발신 금지. `info` "Preloading next track ..." 발신. null이면 `info` "No next track to preload".
  5. 현재 트랙이 시간 있는 이미지면 이미지 타이머 시작 (§2.6).
- 발신 위치: `playlistPlay`(재생 시작), `play`(플레이리스트 모드에서 재생 버튼), `setNext`/`setPrevious`(사용자 트랙 이동), `playNextTrack`.

#### `preload_next` — standby 덱 프리로드만
```json
{
  "command": "preload_next",
  "next": { "...": "file 객체" },
  "next_track_idx": 3,
  "next_time": 5
}
```
- `next`: 프리로드할 트랙. 없으면(null) `info` "No next file to preload" 후 무시.
- `next_track_idx`: 그 트랙의 인덱스. 내부 `next_track_index` 로 저장 — 이후 `next` 명령이 이 값을 새 `track_index` 로 사용한다.
- `next_time`: 초 단위 이미지 시간(이미지일 때만 존재).
- 동작: standby 덱(= 활성 반대 덱)에 `set_media`. `media_changed` 발신 금지. `info` "Preloading updated next track ..." 발신.
- 발신 위치: `end_reached` 처리에서 `next` 명령 직후(새로운 다음 트랙 준비), 재생 중 플레이리스트 편집 시.

#### `next` — 프리로드된 standby 덱으로 스왑
```json
{"command":"next"}
```
- 플레이리스트 모드가 아니면 `error` 후 무시.
- 동작 순서 (피드백 순서 계약):
  1. 이미지 타이머 정지(활성 중이면).
  2. `track_index = next_track_index` → `{"type":"track_index","data":{"value":N}}` 발신.
  3. `active_player_id = next_player_index`(반대 덱) → `{"type":"active_player_id","data":{"value":0|1}}` 발신. **매 스왑마다 0↔1 교대 필수** (§4-②).
  4. standby 덱의 로드된 파일 기동: 이미지면 표시(`media_changed` + `player_data(display_image)` 발신 포함), 비디오/오디오면 재생 시작 후 `media_changed` 발신 — 이때 `media_changed.data` 에 `playlist_track_index` 포함:
     ```json
     {"type":"media_changed","data":{"idx":1,"uuid":"...","path":"C:\\...","playlist_track_index":2}}
     ```
  5. fade_transition: 이전 덱 조용히 정지, 로고 규칙 적용(§2.7) → `logo_visibility` 발신 → `active_player_id` 재발신(동일 값).
  6. 새 트랙이 시간 있는 이미지면 이미지 타이머 시작.
- **주의:** `next` 는 다음 트랙을 새로 로드하지 않는다. 호스트가 `next` 직후 별도로 `preload_next` 를 보내온다.
- 발신 위치: parser `handleEndReached` (repeat `none` 에서 마지막 트랙이 아닐 때, repeat `all` 에서 항상).

#### `previous` — 이전 트랙 (레거시)
```json
{"command":"previous"}
```
- 플레이리스트 모드가 아니면 `error`.
- 현재 활성 덱의 재생 시간이 **5000ms 초과**면 트랙을 바꾸지 않고 처음(0ms)으로 되감기만 한다.
- 그 외: `track_index - 1` (0 미만이면 마지막 트랙으로 순환) 후 내부 `tracks` 배열 기반 `playlist_play` 경로로 재생.
- **현재 호스트는 이 명령을 보내지 않는다** (호스트 `setPrevious` 는 자체 계산 후 `play_current_and_load_next` 를 보냄). 호환성 위해 구현.

#### `playlist_play` — 내부 tracks 기반 재생 (레거시)
```json
{"command":"playlist_play","idx":0}
```
- `idx`: 시작 트랙 인덱스 (누락 시 0). `set_tracks` 로 받은 내부 배열 기준. 범위 밖이거나 목록이 비면 `error`.
- 동작: `track_index` 피드백 → 해당 트랙을 `playid` 경로로 재생 → 내부 배열에서 다음 트랙(`track_index+1`, 끝이면 0으로 순환)을 standby 덱에 자동 프리로드 → 이미지 타이머.
- **현재 호스트는 이 명령을 보내지 않는다** (`playlistPlay` API는 `set_tracks` + `play_current_and_load_next` 사용). 호환성 위해 구현.

#### `set_track_index` — 트랙 인덱스 설정 (레거시)
```json
{"command":"set_track_index","index":2}
```
- 내부 `tracks` 범위 검증(`0 <= index < len(tracks)`) 실패 시 `error`. 성공 시 `{"type":"track_index","data":{"value":2}}` 발신. 재생에는 영향 없음.
- **현재 호스트는 이 명령을 보내지 않는다.** 호환성 위해 구현.

---

### 2.6 이미지 트랙 규칙

- 표시 시간 단위는 **초** (`file.time`, `current_time`, `next_time`). 프로토콜의 다른 모든 시간(ms)과 다르므로 주의 (§4-⑤).
- `time == 0`: 무한 표시. 타이머 없음, `player_data` 는 `duration: 0`, `state: "displaying_image_infinite"`.
- `time` 부재: 기본 5초.
- `time < 0`: 기본 5초로 대체.
- 타이머는 **플레이리스트 모드에서만** 동작한다 (비 플레이리스트 모드에서 이미지는 무기한 표시되고 `end_reached` 를 내지 않는다).
- 타이머 만료 시 → `end_reached` 발신 (§3.6). 자동으로 다음 트랙으로 넘어가지 **않는다** — 호스트가 `next` 를 보내줄 때까지 대기.

### 2.7 화면/로고 자동 규칙 (fade_transition)

재생 전환(playid / play / next / play_current_and_load_next) 시마다 새 미디어 타입에 따라:

| 미디어 타입 | 판정 | 화면 | 로고 | 피드백 |
|---|---|---|---|---|
| 이미지 | `is_image == true` | 이미지 위젯 표시 | 숨김 | `logo_visibility {"show":false}` |
| 오디오 | `mimetype` 이 `"audio/"` 로 시작 | 모든 재생 위젯 숨김 | **표시** | `logo_visibility {"show":true}` |
| 비디오 | 그 외 | 비디오 위젯 표시 | 숨김 | `logo_visibility {"show":false}` |

이전 덱은 **조용히**(어떤 `player_data` 도 발신하지 않고) 정지·숨김 처리한다.

### 2.8 UI 명령

#### `set_fullscreen`
```json
{"command":"set_fullscreen","value":true}
```
- `value`: bool (누락 시 false). 창을 전체화면/일반으로 전환.
- 피드백: `{"type":"set_fullscreen","data":{"value":true}}` — 호스트가 이를 받아 DB에 영속화하므로 **반드시 발신**.

#### `background_color`
```json
{"command":"background_color","color":"#000000"}
```
- `color`: CSS 색상 문자열 (누락 시 `"#000000"`). 창과 재생 위젯 배경색.
- **피드백 없음** (parser에 `set_background` 핸들러가 있으나 현행 플레이어는 발신하지 않는다 — §3.9).

---

## 3. 피드백 (플레이어 → 호스트)

모든 피드백은 `{"type":"<종류>","data":<페이로드>}` JSON 한 줄이다. `port` 만 stdout, 나머지 전부 TCP.

### 3.0 피드백 요약표

| type | data | 발생 시점 | parser.js 소비 |
|---|---|---|---|
| `port` | `{"port":int}` | 기동 직후, **stdout 1회** | TCP 접속 개시 |
| `info` | string | 수시 | 로그. **`'ready'` 포함 시 초기화 트리거** |
| `warn` | string | 수시 | 로그 |
| `debug` | string | 수시 | 로그 |
| `error` | string | 오류 시 | 로그 |
| `player_data` | 상태 객체 | 100ms 틱 + 이미지 이벤트 | `pStatus.player` 갱신 → UI 브로드캐스트 |
| `media_changed` | `{idx,uuid,path,playlist_track_index?}` | 활성 덱 미디어 변경 시 | uuid로 DB 조회 → `pStatus.file`/`trackId` 갱신 |
| `end_reached` | `{playlist_track_index,active_player_id}` | 미디어 종료/이미지 타이머 만료 | 반복/진행 판단 후 명령 회신 |
| `audiodevices` | `{"devices":[{deviceId,name}]}` | `get_audio_devices` 응답 | 목록 저장, 저장된 장치 재적용 |
| `active_player_id` | `{"value":0\|1}` | 활성 덱 변경 시 | `pStatus.activePlayerId` 갱신 |
| `track_index` | `{"value":int}` | 트랙 인덱스 변경 시 | `pStatus.trackId` 갱신 |
| `logo_visibility` | `{"show":bool}` | 로고 상태 변경 시 | `pStatus.logoShow` 갱신 |
| `set_fullscreen` | `{"value":bool}` | `set_fullscreen` 처리 후 | DB 영속화 + 갱신 |
| `set_background` | `{"background":string}` | **현행 플레이어 미발신(예약)** | 발신 시 DB 영속화 |
| `closed` | 임의 | **현행 플레이어 미발신(예약)** | 발신 시 호스트가 `app.exit(0)` |

### 3.1 `port` (stdout 전용, 1회)

```json
{"type":"port","data":{"port":1300}}
```
§1.2 참조. TCP로는 절대 보내지 않는다.

### 3.2 로그 채널: `info` / `warn` / `debug` / `error`

```json
{"type":"info","data":"Player ready"}
{"type":"error","data":"Unknown command: foo"}
```
- `data` 는 **문자열**이어야 한다. (현행 Python에 `get_audio_devices` 실패 시 `{"message": "..."}` 객체를 보내는 quirk가 1곳 있으나, 호스트 로그에 `[object Object]` 로 찍힐 뿐이다. vplayer는 항상 문자열로 통일할 것.)
- `info` 는 단순 로그가 아니다 — parser.js:192:
  ```js
  if (typeof msgData === 'string' && msgData.includes('ready')) {
  ```
  이 조건을 만족하는 `info` 가 올 때마다 `handleReady()` (초기화 명령 일괄 송신)가 재실행된다. §4-① 참조.

### 3.3 `player_data` — 재생 상태 (100ms 틱 + 이벤트)

**틱 규칙:** 100ms 주기로 **활성 덱(`active_player_id`) 하나만** 보고한다. 보고 조건:

- 비디오/오디오: 재생 중(`is_playing`)일 때만. 일시정지/정지 중에는 틱 없음.
- 이미지(타이머 동작 중): 경과 시간 합성해 보고.
- 이미지(무한, `time==0`): 고정값 보고.
- 이미지(시간 만료 후/타이머 없음), 정지 상태: 틱 없음.

**비디오/오디오 틱:**
```json
{"type":"player_data","data":{
  "id": 0,
  "event": "TimeChanged",
  "time": 15234,
  "duration": 180000,
  "position": 0.0846,
  "is_playing": 1,
  "state": "State.Playing"
}}
```

**이미지 틱 (타이머 동작 중):** 이미지에는 실제 재생 시간이 없으므로 타이머로부터 **합성**한다: `time = (표시시간 - 남은시간) ms`, `duration = 표시시간 * 1000 ms`, `position = 경과/전체`.
```json
{"type":"player_data","data":{
  "id": 1,
  "event": "TimeChanged",
  "time": 2300,
  "duration": 10000,
  "position": 0.23,
  "is_playing": true,
  "state": "displaying_image"
}}
```

**이미지 틱 (무한 표시):**
```json
{"type":"player_data","data":{
  "id": 1, "event": "TimeChanged",
  "time": 0, "duration": 0, "position": 0,
  "is_playing": true, "state": "displaying_image_infinite"
}}
```

**이벤트성 발신 (틱과 별개, 1회):**

- 이미지 표시 시작 시 (`event: "display_image"`, `media` 필드 포함, `duration` 0 = 무한):
```json
{"type":"player_data","data":{
  "id": 0, "event": "display_image",
  "media": "C:\\vp\\media\\...\\photo.jpg",
  "state": "displaying_image",
  "time": 0, "duration": 10000, "position": 0, "is_playing": 1
}}
```
- 이미지 정지 시 (`event: "stop_image"`):
```json
{"type":"player_data","data":{
  "id": 0, "event": "stop_image",
  "media": "", "state": "stopped_image",
  "time": 0, "duration": 0, "position": 0, "is_playing": 0
}}
```

**필드 정의:**

| 필드 | 타입 | 의미 |
|---|---|---|
| `id` | int 0\|1 | 덱 번호 |
| `event` | string | 틱은 `"TimeChanged"`, 그 외 `"display_image"`/`"stop_image"` |
| `time` | int ms | 현재 재생 위치 |
| `duration` | int ms | 전체 길이. 이미지 무한 표시는 `0` |
| `position` | float 0–1 | 진행률 |
| `is_playing` | bool 또는 0/1 | 재생 중 여부 (현행은 혼용 — parser는 truthy 판정만 하므로 아무거나 무방, **bool 통일 권장**) |
| `state` | string | 상태 문자열. 비디오는 VLC의 `"State.Playing"` 류, 이미지는 `"displaying_image"` / `"displaying_image_infinite"` / `"stopped_image"`. **parser는 state를 읽지 않으므로** vplayer는 자체 문자열(예: `"playing"`)을 써도 되나 로그 일관성을 위해 유지 권장 |
| `media` | string | `display_image`/`stop_image` 이벤트에만 존재 (틱에는 없음) |

**parser 소비 (알아야 할 quirk):**
```js
if (msgData.id === pStatus.activePlayerId || !pStatus.activePlayerId) { ... }
```
- `activePlayerId` 가 0이면 falsy라서 **모든 id의 player_data가 수용**된다. 따라서 vplayer도 반드시 활성 덱만 틱을 보내야 한다 (standby 덱 틱을 보내면 호스트 상태가 오염됨).
- 병합이 `msgData.time || pStatus.player.time` 식(`||`)이라 **0 값은 기존 값을 덮어쓰지 못한다** (time/duration/position 공통). `is_playing` 만 `!== undefined` 검사로 false 반영 가능. 즉 "정지했음"을 알리는 수단은 값 0이 아니라 **틱 중단 + stop 계열 피드백**이다.

### 3.4 `media_changed`

```json
{"type":"media_changed","data":{
  "idx": 0,
  "uuid": "3f2a9c1e-8b4d-4e0f-9a17-c5d2e6b81a90",
  "path": "C:\\vp\\media\\...\\opening.mp4"
}}
```
플레이리스트 모드일 때는 `playlist_track_index` 가 **추가**된다 (아닐 때는 키 자체가 없음):
```json
{"type":"media_changed","data":{"idx":1,"uuid":"...","path":"...","playlist_track_index":2}}
```
- 발생: **활성 덱**의 미디어가 설정/기동될 때 (`set_media`(활성 덱), `playid`, `next`, `play_current_and_load_next`). **standby 덱 프리로드 시에는 절대 발신 금지.**
- `uuid`: 명령의 `file.uuid` 를 그대로 반사 (없으면 빈 문자열).
- parser 소비: `uuid` 로 DB(`dbFiles`) 조회 → `pStatus.file` 갱신. `playlist_track_index` 가 number이고 플레이리스트 모드이며 `tracks[playlist_track_index].uuid === uuid` 일 때만 `pStatus.trackId` 갱신. uuid가 없거나 DB에 없으면 아무 일도 안 일어난다.

### 3.5 `audiodevices`

```json
{"type":"audiodevices","data":{"devices":[
  {"deviceId":"", "name":"기본 장치"},
  {"deviceId":"{0.0.0.00000000}.{a1b2...}", "name":"스피커 (Realtek(R) Audio)"}
]}}
```
- `get_audio_devices` 명령의 응답. `devices` 는 `{deviceId: string, name: string}` 배열.
- 현행 규칙: 장치 ID가 없으면 `deviceId: "default"`, 설명이 없으면 `name: "기본 장치"`. 기본 장치를 나타내는 항목의 `deviceId` 는 빈 문자열이어도 된다(`set_audio_device` 의 빈 문자열 = 기본 장치와 대칭).
- parser 소비: 목록 저장·UI 전파 후, `pStatus.audioDevice` 가 설정되어 있으면 곧바로 `set_audio_device` 명령을 회신한다. → **vplayer는 이 피드백을 보낸 뒤 곧바로 도착하는 `set_audio_device` 를 처리할 수 있어야 한다.**

### 3.6 `end_reached` — 유일한 진행 트리거

```json
{"type":"end_reached","data":{
  "playlist_track_index": 2,
  "active_player_id": 1
}}
```
- 발생: (a) 비디오/오디오 재생이 끝(EOS)에 도달했을 때, (b) 이미지 타이머 만료 시. **두 필드 모두 필수.**
- `playlist_track_index`: 현재 내부 `track_index` (비 플레이리스트 모드면 마지막 값 그대로 — 보통 0).
- `active_player_id`: 현재 활성 덱 (0|1).
- **플레이어는 이 보고만 하고 절대 스스로 다음 트랙으로 넘어가지 않는다.** 이후 진행(다음 트랙/반복/정지)은 호스트가 `next` / `stop` / `play` / `stop_all` / `play_current_and_load_next` 명령으로 지시한다.
- parser 소비 (`handleEndReached`): 중복 제거(§4-②) 후 `pStatus.repeat` 값에 따라:
  - 비 플레이리스트: `repeat_one` → `stop`+`play`; 그 외 → `stop`.
  - 플레이리스트 `none`: 마지막 트랙 아니면 `next` + `preload_next`; 마지막이면 `stop_all`.
  - 플레이리스트 `all`: 항상 `next` + `preload_next` (인덱스 순환은 호스트가 계산).
  - 플레이리스트 `single`: `stop {idx}`.
  - 플레이리스트 `repeat_one`: `stop {idx}` + `play {idx}`.

### 3.7 `active_player_id`

```json
{"type":"active_player_id","data":{"value":1}}
```
- 발생: 활성 덱이 바뀌는 모든 지점 — 초기화, `playid`(덱 선택 직후), `next`(스왑 시), fade_transition 말미(같은 값 중복 발신 있음 — 무해).
- parser 소비: `pStatus.activePlayerId` 갱신. 이 값은 이후 호스트가 보내는 `play`/`pause`/`stop`/`set_time`/`set_media` 의 `idx` 로 되돌아온다. **§4-② 의 dedup 키 구성 요소.**

### 3.8 `track_index`

```json
{"type":"track_index","data":{"value":3}}
```
- 발생: 트랙 인덱스가 바뀔 때 — `play_current_and_load_next`, `next`, `set_track_index`, (레거시) `playlist_play`/`previous`.
- parser 소비: `pStatus.trackId` 갱신 (호스트의 다음 트랙 계산 기준값).

### 3.9 `logo_visibility` / `set_fullscreen` / `set_background`

```json
{"type":"logo_visibility","data":{"show":false}}
{"type":"set_fullscreen","data":{"value":true}}
{"type":"set_background","data":{"background":"#101010"}}
```
- `logo_visibility`: `show_logo` 처리 후, fade_transition(§2.7) 시, `stop`/`stop_all` 후(로고 복귀) 발신. parser → `pStatus.logoShow`.
- `set_fullscreen`: `set_fullscreen` 명령 처리 후 발신. parser → `pStatus.fullscreen` + DB 영속화. **미발신 시 설정이 저장되지 않으므로 필수.**
- `set_background`: parser 핸들러는 존재하지만 **현행 플레이어는 발신하지 않는다.** vplayer도 v1에서는 발신하지 않는다 (발신하면 DB 영속화가 일어나므로 형태만 예약).

### 3.10 `closed` (예약)

parser 는 `closed` 수신 시 `app.exit(0)` 를 즉시 호출한다. 현행 플레이어는 발신하지 않고 프로세스 종료(→ 호스트 `close` 이벤트 → `app.quit()`)로 갈음한다. vplayer도 발신하지 않는 것을 권장 (즉시 종료가 필요할 때만 사용).

---

## 4. 호환성 필수 준수 사항 (landmines)

### ① ready 판정은 `info` 문자열의 `'ready'` 부분 문자열 매치

`src/player/parser.js` 189–195행 (원문 인용):

```js
case 'info':
  logger.info(`[Player] ${msgData}`)
  // 특별한 info 메시지 처리 (예: ready 이벤트)
  if (typeof msgData === 'string' && msgData.includes('ready')) {
    handleReady()
  }
  break
```

- vplayer는 초기화 완료 후 **반드시** 다음을 송신해야 한다 (이게 없으면 호스트가 배경색/로고/오디오/플레이리스트 모드 초기화를 영원히 하지 않는다):

```json
{"type":"info","data":"Player ready"}
```

- **첫 TCP 클라이언트 접속이 성립한 뒤에** 보낼 것. (현행 Python은 기동 +500ms 고정 타이머라 접속 전 유실 가능성이 있는 경쟁 조건이 있다. 접속 후 송신이 안전한 상위호환.)
- 부분 문자열 매치이므로 다른 `info` 메시지에 `ready` 가 포함되면(예: `"already playing"` 의 al**ready**) `handleReady()` 가 재실행되어 초기화 명령이 다시 쏟아진다. **ready 통지 외의 `info` 문자열에는 `ready` 라는 부분 문자열을 절대 넣지 말 것.**

### ② `end_reached` 중복 제거 키 = `` `${playlist_track_index}-${active_player_id}` ``

`src/player/parser.js` 44–50행:

```js
function handleEndReached(data) {
  const eventKey = `${data.playlist_track_index}-${data.active_player_id}`
  if (lastEndReachedEvent === eventKey) {
    logger.warn(`Duplicate end_reached event ignored: ${eventKey}`)
    return
  }
  lastEndReachedEvent = eventKey
```

- 직전 이벤트와 키가 같으면 **버려진다.** 따라서 `next` 로 덱을 스왑할 때마다 `active_player_id` 가 **반드시 0↔1 교대**되어야 한다. 교대되지 않으면(예: 단일 덱 구현 + 같은 트랙 반복) 연속 `end_reached` 가 같은 키가 되어 두 번째부터 무시되고 플레이리스트 진행이 멈춘다. 이것이 듀얼 덱 구조가 프로토콜 요구사항인 이유다.
- 알려진 한계(현행에도 존재): 플레이리스트 `repeat_one` 은 `stop`+`play` 로 같은 덱·같은 인덱스를 재재생하므로 두 번째 `end_reached` 가 동일 키로 dedup 된다. vplayer가 임의로 우회하지 말고 현행 동작을 유지할 것 (수정은 호스트 측 과제).
- `end_reached` 자체를 미디어 하나당 정확히 1회만 발신할 것 (EOS 이벤트 중복 발화 금지 — dedup은 안전망이지 면죄부가 아니다).

### ③ stdout은 포트 핸드셰이크 1줄만

- stdout에는 `{"type":"port","data":{"port":N}}\n` **단 한 줄**을 쓰고 flush 한 뒤 다시는 쓰지 않는다. GStreamer/서드파티 라이브러리가 stdout에 찍는 로그를 반드시 차단/리다이렉트할 것 (JSON이 아닌 라인이 포트 라인과 같은 청크에 섞이면 호스트의 stdout 파서가 `catch` 로 빠져 핸드셰이크가 실패한다).
- 디버그 로그는 TCP `debug` 피드백 또는 stderr/파일로.

### ④ 트랙 진행·반복 판단은 전적으로 호스트 책임

- 플레이어는 EOS/이미지 타이머 만료 시 `end_reached` 를 **보고만** 하고 대기한다. 절대 자체적으로 다음 트랙 재생, 반복, 정지 전환을 하지 않는다.
- 다음에 무엇을 할지는 호스트가 `next`, `preload_next`, `stop`, `play`, `stop_all`, `play_current_and_load_next` 로 지시한다. repeat 모드(`none`/`all`/`single`/`repeat_one`)는 플레이어에 전달조차 되지 않는다.
- 예외적으로 레거시 명령(`playlist_play`, `previous`)만 내부 `tracks` 배열로 스스로 프리로드하지만, 이 경우에도 트랙 "진행"은 여전히 `end_reached` → 호스트 지시 구조다.

### ⑤ 시간 단위 계약

| 항목 | 단위 |
|---|---|
| `set_time.time`, `player_data.time`, `player_data.duration` | **밀리초 (int)** |
| `player_data.position` | **0–1 float** |
| `file.time`, `current_time`, `next_time` (이미지 표시 시간) | **초 (number)** — ms 아님! |
| 이미지 무한 표시 | `time == 0` (초) → `player_data.duration == 0` (ms) |

혼동 주의: 프로토콜의 재생 시간축은 ms, 이미지 표시 시간만 초 단위다.

### ⑥ 기타 재현해야 할 세부 동작

- 옵션 필드는 **키 부재**로 온다 (`playerSend` 가 undefined 삭제). 단 `play_current_and_load_next.next` 는 `null` 로 올 수 있다. 파서는 "키 없음 / null / 값 있음" 세 경우 모두 견뎌야 한다.
- `media_changed` 는 활성 덱에서만, `playlist_track_index` 는 플레이리스트 모드일 때만 포함(아니면 키 생략).
- standby 덱 프리로드는 어떤 사용자 가시 피드백도 내지 않는다 (`media_changed`/`player_data` 금지, `info` 로그만).
- `player_data` 틱은 활성 덱 & 재생 중일 때만. `pause`/`stop` 상태에서 틱을 계속 보내면 안 된다.
- `pause` 는 `idx` 를 무시하고 활성 덱 토글 (현행 quirk 유지).
- `stop`/`stop_all` 후에는 반드시 `logo_visibility {"show":true}` 를 보낸다.
- 알 수 없는 명령/필드 오류는 `error` 피드백 후 계속 동작 (프로세스 유지).

---

## 5. v2 확장 예정 (구현 전 — 설계 예약)

**원칙: 기존 v1 명령·피드백의 이름/필드/의미는 절대 변경하지 않는다. 확장은 새 command / 새 feedback type의 추가로만 한다.** v1 호스트는 모르는 피드백 type을 `Unknown message type` 경고로 무시하고, v1 플레이어는 모르는 명령을 `error` 로 무시하므로 양방향 전방 호환이 성립한다.

| 구분 | 이름 (예약) | 방향 | 페이로드 (안) | 용도 / 단계 |
|---|---|---|---|---|
| 명령 | `probe_media` | H→P | `{file}` → 피드백 `media_probed {uuid, duration, codec, resolution, ...}` | 미디어 메타데이터 조사 (Phase 2.5) |
| 명령 | `make_thumbnail` | H→P | `{file, out_path, at_ms?}` → 피드백 `thumbnail_ready` | 썸네일 생성 (Phase 2.5) |
| 피드백 | `capabilities` | P→H | `{hw_decoders:[...], max_decode:"4k60", apis:["d3d11va","nvdec",...]}` | 기동 시 HW 디코드 프로브 결과 보고 |
| 명령 | `audio_track_play` | H→P | `{track_id, file, device_id?, volume?, channel_map?}` | 독립 오디오 트랙 재생 (본 영상과 별개 버스) |
| 명령 | `audio_track_stop` / `audio_track_pause` | H→P | `{track_id}` | 독립 오디오 트랙 제어 |
| 명령 | `audio_track_set_volume` | H→P | `{track_id, volume: 0-100}` | 트랙 볼륨 |
| 명령 | `audio_track_set_channel_map` | H→P | `{track_id, map:[출력채널 인덱스,...]}` | 채널 라우팅 |
| 피드백 | `audio_track_data` | P→H | `{track_id, time, duration, position, is_playing, state}` (단위는 v1과 동일: ms / 0–1) | 독립 오디오 트랙 상태 틱 |
| 명령 | `set_timeline` | H→P | `{timeline_id, cues:[{at_ms, action, ...}]}` | 타임라인(큐 시트) 로드 |
| 명령 | `timeline_play` / `timeline_pause` / `timeline_seek` | H→P | `{timeline_id}` / `{timeline_id}` / `{timeline_id, time_ms}` | 타임라인 트랜스포트 |
| 피드백 | `timeline_cue_ready` | P→H | `{timeline_id, cue_id}` | 큐 프리롤 완료 |
| 피드백 | `timeline_cue_late` | P→H | `{timeline_id, cue_id, late_ms}` | 큐 지연 발화 경고 |
| 피드백 | `timeline_position` | P→H | `{timeline_id, time_ms}` | 타임라인 위치 틱 |
| 명령 | `get_audio_device_caps` | H→P | 응답 피드백 `audio_device_caps {"devices":[{deviceId, name, type:"wasapi"\|"asio", channels:int}]}` — WASAPI는 8ch 기준, ASIO는 드라이버가 보고하는 채널 수 | 멀티채널 출력 능력 조회 (v1 `get_audio_devices`/`audiodevices` 는 그대로 유지) |

### 5.1 v2 구현 명세 — 멀티채널 라우팅 + 독립 오디오 트랙 (2026-07-10 구현 완료)

위 표에서 `audio_track_*`, `get_audio_device_caps`, `capabilities` 는 구현 완료. 타임라인
(`set_timeline`/`timeline_*`) 은 계속 예약 상태. 확정 시맨틱:

**오디오 버스 정책.** 믹서 출력 채널수 N 은 `set_audio_device` 시 디바이스를 따른다 —
wasapi = min(디바이스 채널, 8) + positioned(표준 fallback mask) / asio = 드라이버 보고
채널수 + unpositioned(channel-mask=0). 기본(디바이스 미지정) = 2ch stereo. 디바이스
전환은 재생 중에도 안전 (검증: `test/spike_mixcaps.cpp` 2↔128ch, `test/smoke7.js`).
적용 완료 시 `info` 피드백 `"audio device applied: <id> (Nch bus)"`.

**`channel_map` (v1 명령 확장 필드).** 모든 file/track 객체(`playid`, `set_media`,
`play_current_and_load_next` 의 current/next, `preload_next.next`, `set_tracks[i]`)에
옵션 필드로 실린다:
- `channel_map: [int,...]` — 인덱스 = 소스 채널, 값 = 출력(버스) 채널 인덱스, `-1` = 뮤트.
  소스는 map 길이 채널수로 다운/업믹스된 뒤 배치된다 (예: `[4,5]` = 스테레오를 버스 4,5로).
  부재/`null`/빈 배열 = v1 동작 (스테레오 다운믹스 → 버스 0,1).
- `volume: 0-100` — 덱 볼륨 (기본 100). 로드 시 적용 + `set_deck_audio`로 라이브 변경 가능.
- `muted: bool` — true면 실효 볼륨 0 (기본 false).

**`embedded_streams` (채널별 라우팅/볼륨/뮤트, `channel_map`의 상위 확장).** 모든 file/track
객체에 옵션으로 실린다. 있으면 `channel_map`/`volume`/`muted`보다 우선:
```
"embedded_streams": [
  { "index":0, "volume":100, "muted":false,          // 스트림(마스터) 볼륨/뮤트
    "channels":[ {"out":0,"volume":100,"muted":false},// 소스 채널별 {출력, gain, 뮤트}
                 {"out":1,"volume":50,"muted":false} ] }
]
```
- `channels[c]` = 소스 채널 c → `{out(-1=뮤트/미라우팅), volume 0-100(=mix-matrix 계수), muted}`.
  실효 = 마스터 볼륨(브랜치 volume 요소) × 채널 gain(matrix 계수).
- 스트림 `volume`/`muted` = 마스터(스트림 전체). 채널 폭 = `channels.length`.
- **현재 플레이어는 `embedded_streams[0]`(첫 스트림)만 처리** — 다중 스트림(다국어)은 Phase D2 예정.
- 부재 시 레거시 `channel_map`/`volume`/`muted` 폴백(스트림0 동작).

**`set_deck_audio {streams?:[{index, volume?, muted?, channels?:[{out,volume?,muted?}]}], channel_map?, volume?, muted?}` (H→P).**
활성 덱(임베디드 오디오)의 라우팅/볼륨/뮤트를 **재생 중 라이브** 변경. `streams[0]`의
`channels`는 채널별 mix-matrix 계수를, `volume`/`muted`는 마스터(volume 요소)를 갱신(부분
갱신 — 준 필드만). 레거시 `{channel_map,volume,muted}`(streams 없이)는 스트림0 전체 대상.
브랜치 채널 폭(로드 시 협상값)은 유지 — 폭 변경은 재로드. 구버전 플레이어는 무시하므로
`capabilities.features` 에 `live_routing` 이 있을 때만 전송할 것.

**독립 오디오 트랙.** 덱과 무관한 오디오 전용 병행 스트림 (동시 최대 8개, 초과 시 `error`).
- `audio_track_play {track_id:string, file, volume?, channel_map?, loop?:bool, muted?:bool}` —
  `volume`/`channel_map`/`muted` 는 명령 레벨 우선, `file` 객체 폴백. `muted:true`면 실효
  볼륨 0. 같은 `track_id` 재호출 = 교체. `loop:true` = 플레이어 측 루프 (EOS 차단 +
  플러시 시크, 갭 수 ms). 비오디오 스트림(MP3 앨범아트 등)은 무시된다.
- `audio_track_stop {track_id}` — 정지 + 해체. `state:"stopped"` 인 `audio_track_data` 1회 발신.
- `audio_track_pause {track_id}` — 토글 (v1 덱 `pause` 와 동일 규약).
- `audio_track_set_volume {track_id, volume:0-100}` — 라이브 적용.
- `audio_track_play` 는 `channels:[{out,volume,muted}]`(채널별) 도 받는다(없으면 `channel_map` 폴백).
- `audio_track_set_channel_map {track_id, map}` — `map` 은 채널별 `[{out,volume,muted}]`(신규)
  또는 레거시 `[int,...]`. 라이브 적용. 브랜치 채널 폭은 `audio_track_play` 시점 고정.
- 피드백 `audio_track_data {track_id, time, duration, position, is_playing, state}` —
  100ms 틱 (재생/일시정지 중), 단위 v1 과 동일 (ms / 0–1). `state`: `"playing"` |
  `"paused"` | `"stopped"`(종료 시 1회). 자연 종료(비루프)도 stopped 1회 후 틱 중단.
- 전역 `stop`/`stop_all` 은 **오디오 트랙에 영향 없음** — 생명주기는 전적으로 호스트가
  `audio_track_stop` 으로 관리한다 (repeat 모드별 정책은 호스트 소관).

**기능 협상.** ready 직후 피드백
`capabilities {"features":["channel_map","audio_track","live_routing","embedded_streams"]}` 발신.
호스트는 이 목록에 있는 기능만 송신한다 (구버전 플레이어 = 목록 부재 = v1 강하).
ready/capabilities 는 **첫 TCP 클라이언트 연결 후** 발신된다 (§4 의 500ms 레이스 해소 —
연결 전이면 500ms 주기로 재시도).

검증: `test/smoke7.js` (라우팅/디바이스 전환), `test/smoke8.js` (병행/루프/수명주기),
`test/smoke9.js` (채널별 gain/mute/라우팅 + 마스터 볼륨/뮤트 라이브 + 레거시 회귀).

---

## 부록 A. 대표 시나리오 메시지 흐름

**플레이리스트 재생 시작 → 트랙 자동 진행 (repeat: all)**

```
H→P {"command":"set_tracks","tracks":[T0,T1,T2]}
H→P {"command":"play_current_and_load_next","current":T0,"next":T1,"track_idx":0}
P→H {"type":"track_index","data":{"value":0}}
P→H {"type":"info","data":"Playing track 0: opening.mp4"}
P→H {"type":"active_player_id","data":{"value":0}}
P→H {"type":"media_changed","data":{"idx":0,"uuid":"<T0.uuid>","path":"<T0.path>","playlist_track_index":0}}
P→H {"type":"logo_visibility","data":{"show":false}}
P→H {"type":"active_player_id","data":{"value":0}}
P→H {"type":"info","data":"Preloading next track 1: photo.jpg"}
P→H {"type":"player_data","data":{"id":0,"event":"TimeChanged","time":100,...}}   (100ms 반복)
        ... T0 재생 끝 ...
P→H {"type":"end_reached","data":{"playlist_track_index":0,"active_player_id":0}}
H→P {"command":"next"}
P→H {"type":"track_index","data":{"value":1}}
P→H {"type":"active_player_id","data":{"value":1}}          ← 덱 스왑 (0→1)
P→H {"type":"media_changed","data":{"idx":1,"uuid":"<T1.uuid>","path":"<T1.path>","playlist_track_index":1}}
P→H {"type":"player_data","data":{"id":1,"event":"display_image","media":"<T1.path>",...}}
P→H {"type":"logo_visibility","data":{"show":false}}
P→H {"type":"active_player_id","data":{"value":1}}
H→P {"command":"preload_next","next":T2,"next_track_idx":2,"next_time":...}
P→H {"type":"info","data":"Preloading updated next track 2: ..."}
        ... 이미지 타이머 만료 ...
P→H {"type":"end_reached","data":{"playlist_track_index":1,"active_player_id":1}}   ← 키 "1-1" ≠ "0-0"
```
