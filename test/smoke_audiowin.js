#!/usr/bin/env node
// vplayer 오디오 전용 창(화면 송출 없음) 스모크.
//
// 사용법: node test/smoke_audiowin.js [vplayer.exe]
//   기본 exe = build/Release/vplayer.exe
//
// 시나리오 (테스트 미디어는 gst-launch로 임시 생성):
//   1) capabilities 에 'audio_only_window' 포함 확인
//   2) create_window {window_id:1, audio_only:true} → windows 목록에 audio_only:true
//   3) 2초 WAV 재생 → media_changed + 오디오 EOS 기반 end_reached
//   4) 2초 영상+오디오(MKV) 재생 → media_changed + end_reached (영상은 선택 해제)
//   5) PNG 이미지 재생 → playback_error{reason:'no_audio_stream'}
//   6) 일반 창(2)에서 WAV 재생 → end_reached (비디오 없는 미디어의 오디오 EOS 보고)
//   7) 일반 창(2)에서 MKV 재생 → end_reached 정확히 1회 (오디오 EOS 중복 보고 없음)
// 검증: 위 관측 전부 + error 피드백 없음.

const { spawn, execFileSync } = require('child_process')
const net = require('net')
const path = require('path')
const fs = require('fs')
const os = require('os')

const exePath = process.argv[2] || path.join(__dirname, '..', 'build', 'Release', 'vplayer.exe')

const gstRoot = process.env.GSTREAMER_1_0_ROOT_MSVC_X86_64 || 'C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64'
const gstBin = path.join(gstRoot, 'bin')
const env = { ...process.env, PATH: gstBin + ';' + (process.env.PATH || '') }
const gstLaunch = path.join(gstBin, 'gst-launch-1.0.exe')

// ---- 테스트 미디어 생성 ----
// gst-launch는 location= 값의 백슬래시를 이스케이프로 해석 → 슬래시 경로 사용
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vp-audiowin-')).split(path.sep).join('/')
const wavPath = `${dir}/tone.wav`
const mkvPath = `${dir}/av.mkv`
const pngPath = `${dir}/still.png`
const gst = (args) => execFileSync(gstLaunch, ['-q', ...args], { env, stdio: 'ignore' })
gst(['audiotestsrc', 'num-buffers=86', '!', 'audioconvert', '!', 'wavenc', '!', 'filesink', `location=${wavPath}`])
gst(['matroskamux', 'name=m', '!', 'filesink', `location=${mkvPath}`,
  'videotestsrc', 'num-buffers=60', '!', 'video/x-raw,width=320,height=240,framerate=30/1', '!', 'x264enc', '!', 'm.',
  'audiotestsrc', 'num-buffers=86', '!', 'audioconvert', '!', 'vorbisenc', '!', 'm.'])
gst(['videotestsrc', 'num-buffers=1', '!', 'pngenc', '!', 'filesink', `location=${pngPath}`])

const errors = []
const ended = [] // {window_id, track}
const changed = [] // {window_id, uuid}
const playbackErrors = [] // {window_id, reason}
let sawCap = false
let sawAudioOnlyInList = false

const player = spawn(exePath, [], { env, stdio: ['pipe', 'pipe', 'pipe'] })
player.stderr.on('data', (d) => process.stderr.write(`[stderr] ${d}`))
player.on('close', () => report())
player.on('error', (e) => { errors.push('player spawn: ' + e.message); report() })

let stdoutBuf = ''
player.stdout.on('data', (data) => {
  stdoutBuf += data.toString()
  const nl = stdoutBuf.indexOf('\n')
  if (nl === -1) return
  try {
    const msg = JSON.parse(stdoutBuf.slice(0, nl).trim())
    if (msg.type === 'port') connect(msg.data.port)
  } catch { /* ignore */ }
})

function connect(port) {
  const sock = net.connect(port, '127.0.0.1', () => run(sock))
  let buf = ''
  sock.on('data', (d) => {
    buf += d.toString()
    let i
    while ((i = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, i).trim()
      buf = buf.slice(i + 1)
      if (!line) continue
      let msg
      try { msg = JSON.parse(line) } catch { continue }
      switch (msg.type) {
        case 'capabilities':
          sawCap = (msg.data.features || []).includes('audio_only_window')
          break
        case 'windows':
          for (const w of msg.data.windows || []) if (w.window_id === 1 && w.audio_only) sawAudioOnlyInList = true
          break
        case 'media_changed':
          changed.push({ window_id: msg.data.window_id, uuid: msg.data.uuid })
          console.log(`[media_changed] win=${msg.data.window_id} uuid=${msg.data.uuid}`)
          break
        case 'end_reached':
          ended.push({ window_id: msg.data.window_id, track: msg.data.playlist_track_index })
          console.log(`[end_reached] ${JSON.stringify(msg.data)}`)
          break
        case 'playback_error':
          playbackErrors.push(msg.data)
          console.log(`[playback_error] ${JSON.stringify(msg.data)}`)
          break
        case 'error':
          errors.push(msg.data); console.log(`[ERROR] ${msg.data}`); break
        default:
          break
      }
    }
  })
  sock.on('error', () => {})
}

function send(sock, obj) {
  console.log(`[send] ${JSON.stringify(obj).slice(0, 140)}`)
  sock.write(JSON.stringify(obj) + '\n')
}

function run(sock) {
  const play = (wid, idx, p, uuid) =>
    send(sock, { command: 'play_current_and_load_next', window_id: wid, track_idx: idx, current: { path: p, uuid } })
  setTimeout(() => send(sock, { command: 'create_window', window_id: 1, audio_only: true }), 600)
  setTimeout(() => play(1, 0, wavPath, 'WAV'), 1200)
  setTimeout(() => play(1, 1, mkvPath, 'MKV'), 5000)
  setTimeout(() => play(1, 2, pngPath, 'PNG'), 9000)
  setTimeout(() => send(sock, { command: 'create_window', window_id: 2, x: 100, y: 100, width: 320, height: 180 }), 11000)
  setTimeout(() => play(2, 0, wavPath, 'WAV2'), 11800)
  setTimeout(() => play(2, 1, mkvPath, 'MKV2'), 15000)
  setTimeout(() => send(sock, { command: 'destroy_window', window_id: 1 }), 19000)
  setTimeout(() => send(sock, { command: 'stop_all' }), 19500)
  setTimeout(() => { sock.destroy(); player.kill() }, 20300)
}

function report() {
  const endedOn = (w, t) => ended.some((e) => e.window_id === w && e.track === t)
  const changedTo = (w, u) => changed.some((c) => c.window_id === w && c.uuid === u)
  const checks = {
    'capability audio_only_window': sawCap,
    'windows 목록 audio_only:true': sawAudioOnlyInList,
    'WAV media_changed (창1)': changedTo(1, 'WAV'),
    'WAV end_reached (창1)': endedOn(1, 0),
    'MKV media_changed (창1)': changedTo(1, 'MKV'),
    'MKV end_reached (창1)': endedOn(1, 1),
    'PNG no_audio_stream (창1)': playbackErrors.some((e) => e.window_id === 1 && e.reason === 'no_audio_stream'),
    'WAV end_reached (일반 창2)': endedOn(2, 0),
    // 영상+오디오는 비디오 EOS만 보고 — 오디오 EOS 프로브가 중복 end_reached를 내지 않아야 함
    'MKV end_reached 정확히 1회 (일반 창2)': ended.filter((e) => e.window_id === 2 && e.track === 1).length === 1,
    'error 피드백 없음': errors.length === 0,
  }
  const pass = Object.values(checks).every(Boolean)
  console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  for (const [k, v] of Object.entries(checks)) console.log(`${v ? 'ok  ' : 'FAIL'} ${k}`)
  if (errors.length) console.log(JSON.stringify(errors))
  try { fs.rmSync(dir, { recursive: true, force: true }) } catch { /* ignore */ }
  process.exit(pass ? 0 : 1)
}
