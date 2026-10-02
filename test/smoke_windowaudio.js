#!/usr/bin/env node
// vplayer 창별 오디오 디바이스 + 창 뮤트 스모크.
//
// 사용법: node test/smoke_windowaudio.js [vplayer.exe]
//   기본 exe = build/Release/vplayer.exe. WASAPI 출력 디바이스가 2개 이상이면 두 번째 디바이스로
//   디바이스 버스를 검증하고, 1개뿐이면 그 디바이스 id를 지정한다(버스는 생성되지만 같은 장치).
//
// 시나리오:
//   1) capabilities 에 window_audio_device, window_mute
//   2) 창1(기본 디바이스), 창2(디바이스 D, muted:true), 창3(디바이스 D) — 모두 audio_only
//   3) 세 창 WAV 재생 + 창2 귀속 오디오 트랙 → media_changed 3개
//   4) windows 목록: 창2/3 audio_device=D, 창2 muted=true / audio_buses = 2 (main + D 공유)
//   5) set_window_mute 창2 false → window_mute 피드백
//   6) 창2·3 파괴 후 트랙 정지 → 디바이스 버스 해체(audio_buses=1)
// 검증: 위 관측 + error 없음.

const { spawn, execFileSync } = require('child_process')
const net = require('net')
const path = require('path')
const fs = require('fs')
const os = require('os')

const exePath = process.argv[2] || path.join(__dirname, '..', 'build', 'Release', 'vplayer.exe')
const gstRoot = process.env.GSTREAMER_1_0_ROOT_MSVC_X86_64 || 'C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64'
const gstBin = path.join(gstRoot, 'bin')
const env = { ...process.env, PATH: gstBin + ';' + (process.env.PATH || '') }

// gst-launch는 location= 값의 백슬래시를 이스케이프로 해석 → 슬래시 경로
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vp-winaudio-')).split(path.sep).join('/')
const wav = `${dir}/tone.wav`
execFileSync(path.join(gstBin, 'gst-launch-1.0.exe'),
  ['-q', 'audiotestsrc', 'num-buffers=300', '!', 'audioconvert', '!', 'wavenc', '!', 'filesink', `location=${wav}`],
  { env, stdio: 'ignore' })

const errors = []
const changed = new Set()
let caps = []
let dev = null
let devCount = 0
let winSnap = null
const busCounts = []
const muteEvents = []

const player = spawn(exePath, [], { env, stdio: ['pipe', 'pipe', 'pipe'] })
player.on('close', () => report())
player.on('error', (e) => { errors.push('spawn: ' + e.message); report() })

let outBuf = ''
player.stdout.on('data', (d) => {
  outBuf += d
  const i = outBuf.indexOf('\n')
  if (i < 0) return
  try { const m = JSON.parse(outBuf.slice(0, i)); if (m.type === 'port') connect(m.data.port) } catch { /* ignore */ }
})

function connect(port) {
  const sock = net.connect(port, '127.0.0.1', () => run(sock))
  let buf = ''
  sock.on('data', (d) => {
    buf += d
    let j
    while ((j = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, j); buf = buf.slice(j + 1)
      let m; try { m = JSON.parse(line) } catch { continue }
      switch (m.type) {
        case 'capabilities': caps = m.data.features || []; break
        case 'audiodevices': {
          const wasapi = (m.data.devices || []).filter((x) => !String(x.deviceId).startsWith('asio:') && x.deviceId)
          devCount = wasapi.length
          dev = (wasapi[1] || wasapi[0])?.deviceId || null
          console.log(`[devices] ${wasapi.map((x) => `${x.name} [${x.channels}ch]`).join(' | ')}`)
          break
        }
        case 'windows':
          if (!m.data.created && !m.data.destroyed) winSnap = m.data.windows
          break
        case 'memory_status':
          if (Number.isInteger(m.data.audio_buses) && busCounts[busCounts.length - 1] !== m.data.audio_buses) {
            busCounts.push(m.data.audio_buses); console.log(`[audio_buses] ${m.data.audio_buses}`)
          }
          break
        case 'media_changed': changed.add(m.data.window_id); break
        case 'window_mute': muteEvents.push(m.data); console.log(`[window_mute] ${JSON.stringify(m.data)}`); break
        case 'error': errors.push(m.data); console.log(`[ERROR] ${m.data}`); break
        case 'info': if (/bus/.test(m.data)) console.log(`[info] ${m.data}`); break
        case 'debug': if (/bus/.test(m.data)) console.log(`[debug] ${m.data}`); break
        default: break
      }
    }
  })
  sock.on('error', () => {})
}

function run(sock) {
  const send = (o) => sock.write(JSON.stringify(o) + '\n')
  setTimeout(() => send({ command: 'get_audio_devices' }), 700)
  setTimeout(() => {
    send({ command: 'create_window', window_id: 1, audio_only: true })
    send({ command: 'create_window', window_id: 2, audio_only: true, audio_device: dev, muted: true })
    send({ command: 'create_window', window_id: 3, audio_only: true, audio_device: dev })
  }, 1500)
  setTimeout(() => {
    for (const w of [1, 2, 3])
      send({ command: 'play_current_and_load_next', window_id: w, track_idx: 0, current: { path: wav, uuid: `W${w}` } })
    send({ command: 'audio_track_play', track_id: 't2', window_id: 2, file: { path: wav } })
  }, 2200)
  setTimeout(() => send({ command: 'get_windows' }), 3300)
  setTimeout(() => send({ command: 'set_window_mute', window_id: 2, muted: false }), 3600)
  setTimeout(() => { send({ command: 'destroy_window', window_id: 2 }); send({ command: 'destroy_window', window_id: 3 }) }, 4500)
  setTimeout(() => send({ command: 'audio_track_stop', track_id: 't2' }), 5000)
  setTimeout(() => { sock.destroy(); player.kill() }, 7000)
}

function report() {
  const w = (id) => (winSnap || []).find((x) => x.window_id === id) || {}
  const checks = {
    'capabilities window_audio_device/window_mute': caps.includes('window_audio_device') && caps.includes('window_mute'),
    '출력 디바이스 조회': !!dev,
    'media_changed 창1/2/3': [1, 2, 3].every((id) => changed.has(id)),
    '창1 = main 버스': w(1).audio_device === '',
    '창2/3 = 지정 디바이스': w(2).audio_device === dev && w(3).audio_device === dev,
    '창2 muted:true (create_window muted)': w(2).muted === true,
    '디바이스 버스 공유 (audio_buses 2)': busCounts.includes(2) && !busCounts.includes(3),
    'set_window_mute 해제 피드백': muteEvents.some((e) => e.window_id === 2 && e.muted === false),
    '창·트랙 정리 후 버스 해체 (audio_buses 1)': busCounts[busCounts.length - 1] === 1,
    'error 없음': errors.length === 0,
  }
  const pass = Object.values(checks).every(Boolean)
  console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} === (WASAPI 디바이스 ${devCount}개)`)
  for (const [k, v] of Object.entries(checks)) console.log(`${v ? 'ok  ' : 'FAIL'} ${k}`)
  if (errors.length) console.log(JSON.stringify(errors))
  try { fs.rmSync(dir, { recursive: true, force: true }) } catch { /* ignore */ }
  process.exit(pass ? 0 : 1)
}
