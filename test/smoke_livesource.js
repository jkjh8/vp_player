#!/usr/bin/env node
// vplayer 라이브 입력 소스(창 귀속 지속 레이어) 스모크 — RTP 루프백 (실장비 불요).
//
// 사용법: node test/smoke_livesource.js [vplayer.exe]
//   기본 exe = build/Release/vplayer.exe
//
// 시나리오:
//   0) 로컬 RTP 송신기 spawn: videotestsrc ! x264enc ! rtph264pay ! udpsink :5004
//   1) capabilities 에 'live_source' 포함 확인
//   2) create_window {1}
//   3) set_window_source {window_id:1, source:{kind:'rtp', rtp:{port:5004,encoding_name:'H264',
//      payload:96,clock_rate:90000,media:'video'}}}
//   4) source_status: connecting → playing 관측
//   5) 다른 창(0)에서 플레이리스트가 없어도 라이브 유지 — clear_window_source → cleared
// 검증: capabilities live_source, source_status playing/cleared, 에러 없음.

const { spawn } = require('child_process')
const net = require('net')
const path = require('path')

const exePath = process.argv[2] || path.join(__dirname, '..', 'build', 'Release', 'vplayer.exe')

// GStreamer bin 을 PATH 에 추가 (dev — 시스템 GStreamer 사용)
let gstRoot = process.env.GSTREAMER_1_0_ROOT_MSVC_X86_64 || 'C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64'
const gstBin = path.join(gstRoot, 'bin')
const env = { ...process.env, PATH: gstBin + ';' + (process.env.PATH || '') }
const gstLaunch = path.join(gstBin, 'gst-launch-1.0.exe')

const PORT = 5004
const errors = []
const statuses = [] // {state}
let sawLiveCap = false

// 0) RTP 송신기
const sender = spawn(
  gstLaunch,
  ['-q', 'videotestsrc', 'is-live=true', 'pattern=ball', '!',
   'video/x-raw,width=640,height=480,framerate=30/1', '!', 'x264enc', 'tune=zerolatency',
   'key-int-max=15', '!', 'rtph264pay', 'config-interval=1', 'pt=96', '!',
   `udpsink`, `host=127.0.0.1`, `port=${PORT}`],
  { env, stdio: ['ignore', 'ignore', 'pipe'] },
)
sender.stderr.on('data', (d) => process.stderr.write(`[sender] ${d}`))
sender.on('error', (e) => { errors.push('sender spawn: ' + e.message) })

// 1) vplayer
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
  const sock = net.createConnection({ host: '127.0.0.1', port }, () => run(sock))
  let buf = ''
  sock.on('data', (data) => {
    buf += data.toString()
    let nl
    while ((nl = buf.indexOf('\n')) !== -1) {
      const line = buf.slice(0, nl)
      buf = buf.slice(nl + 1)
      if (!line.trim()) continue
      let msg
      try { msg = JSON.parse(line) } catch { continue }
      switch (msg.type) {
        case 'capabilities':
          sawLiveCap = (msg.data.features || []).includes('live_source')
          console.log(`[capabilities] live_source=${sawLiveCap}`)
          break
        case 'source_status':
          statuses.push(msg.data.state)
          console.log(`[source_status] win=${msg.data.window_id} kind=${msg.data.kind} state=${msg.data.state}${msg.data.reason ? ' reason=' + msg.data.reason : ''}`)
          break
        case 'error':
          errors.push(msg.data); console.log(`[ERROR] ${msg.data}`); break
        case 'warn':
          console.log(`[warn] ${JSON.stringify(msg.data).slice(0, 140)}`); break
        default:
          break
      }
    }
  })
  sock.on('error', () => {})
}

function send(sock, obj) {
  const s = JSON.stringify(obj)
  console.log(`[send] ${s.length > 140 ? s.slice(0, 137) + '...' : s}`)
  sock.write(s + '\n')
}

function run(sock) {
  setTimeout(() => send(sock, { command: 'create_window', window_id: 1, monitor_index: -1, x: 100, y: 100, width: 640, height: 480, aspect_mode: 'letterbox' }), 600)
  setTimeout(() => send(sock, {
    command: 'set_window_source',
    window_id: 1,
    source: { kind: 'rtp', rtp: { port: PORT, encoding_name: 'H264', payload: 96, clock_rate: 90000, media: 'video' }, latency_ms: 100, has_audio: false },
  }), 1200)
  // 라이브 유지 관찰
  setTimeout(() => send(sock, { command: 'clear_window_source', window_id: 1 }), 6000)
  setTimeout(() => { sock.destroy(); player.kill(); try { sender.kill() } catch {} }, 7200)
}

function report() {
  try { sender.kill() } catch {}
  const sawPlaying = statuses.includes('playing')
  const sawCleared = statuses.includes('cleared')
  // 링크 실패/디코더 오류 등 치명 에러만 실패로 (경고 제외)
  const fatal = errors.filter((e) => typeof e === 'string' && !/reconnect/i.test(e))
  const pass = sawLiveCap && sawPlaying && sawCleared && fatal.length === 0
  console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  console.log(`capabilities live_source: ${sawLiveCap}`)
  console.log(`source_status 순서: ${JSON.stringify(statuses)}`)
  console.log(`playing 관측: ${sawPlaying}, cleared 관측: ${sawCleared}`)
  console.log(`에러: ${fatal.length}${fatal.length ? ' ' + JSON.stringify(fatal) : ''}`)
  process.exit(pass ? 0 : 1)
}
