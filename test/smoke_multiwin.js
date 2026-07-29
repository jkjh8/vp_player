#!/usr/bin/env node
// vplayer 멀티 윈도우 동시 재생 스모크 (Phase 1)
//
// 사용법: node test/smoke_multiwin.js <vplayer.exe> <videoA.mp4> <videoB.mp4>
//
// 시나리오:
//   1) get_windows → 주 창(0) 1개
//   2) create_window {window_id:1} → 창 목록 2개
//   3) window 0 에 videoA, window 1 에 videoB 를 동시(play_current_and_load_next) 재생
//   4) 두 창 모두 media_changed(window_id 반영) + player_data(is_playing) 수신 확인
//   5) destroy_window(1) → 창 목록 1개
//   6) stop_all
// 검증: 창 생성/삭제, 창별 media_changed, 창별 동시 player_data 틱.

const { spawn } = require('child_process')
const net = require('net')

const [exePath, vaPath, vbPath] = process.argv.slice(2)
if (!exePath || !vaPath || !vbPath) {
  console.error('usage: node test/smoke_multiwin.js <vplayer.exe> <videoA> <videoB>')
  process.exit(1)
}

const errors = []
const mediaChanged = []                 // {window_id}
const playingWins = new Set()           // player_data is_playing:true 를 낸 window_id
let windowsList = []                    // 최근 get_windows/windows 피드백
let sawTwoWindows = false
let sawOneWindowAfterDestroy = false

const player = spawn(exePath, [], { stdio: ['pipe', 'pipe', 'pipe'] })
player.stderr.on('data', (d) => process.stderr.write(`[stderr] ${d}`))
player.on('close', () => report())

let stdoutBuf = ''
player.stdout.on('data', (data) => {
  stdoutBuf += data.toString()
  const nl = stdoutBuf.indexOf('\n')
  if (nl === -1) return
  const msg = JSON.parse(stdoutBuf.slice(0, nl).trim())
  if (msg.type === 'port') connect(msg.data.port)
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
      const msg = JSON.parse(line)
      switch (msg.type) {
        case 'windows':
          windowsList = msg.data.windows || []
          if (windowsList.length === 2) sawTwoWindows = true
          if (msg.data.destroyed === 1 && windowsList.length === 1) sawOneWindowAfterDestroy = true
          console.log(`[windows] ${JSON.stringify(windowsList.map((w) => w.window_id))} ${JSON.stringify(msg.data)}`)
          break
        case 'media_changed':
          mediaChanged.push(msg.data.window_id)
          console.log(`[media_changed] win=${msg.data.window_id} idx=${msg.data.idx} uuid=${msg.data.uuid}`)
          break
        case 'player_data':
          if (msg.data.is_playing) playingWins.add(msg.data.window_id)
          break
        case 'error':
          errors.push(msg.data); console.log(`[ERROR] ${msg.data}`); break
        default:
          console.log(`[feedback] ${line.slice(0, 140)}`)
      }
    }
  })
  sock.on('error', () => {})
}

function send(sock, obj) {
  const s = JSON.stringify(obj)
  console.log(`[send] ${s.length > 120 ? s.slice(0, 117) + '...' : s}`)
  sock.write(s + '\n')
}

function run(sock) {
  setTimeout(() => send(sock, { command: 'get_windows' }), 500)
  setTimeout(() => send(sock, { command: 'create_window', window_id: 1, monitor_index: -1, x: 120, y: 120, width: 480, height: 270, aspect_mode: 'letterbox' }), 900)
  // 두 창 동시 재생
  setTimeout(() => send(sock, { command: 'play_current_and_load_next', window_id: 0, track_idx: 0, current: { path: vaPath, uuid: 'A' } }), 1500)
  setTimeout(() => send(sock, { command: 'play_current_and_load_next', window_id: 1, track_idx: 0, current: { path: vbPath, uuid: 'B' } }), 1600)
  // 관찰
  setTimeout(() => send(sock, { command: 'destroy_window', window_id: 1 }), 6000)
  setTimeout(() => send(sock, { command: 'stop_all' }), 6600)
  setTimeout(() => { sock.destroy(); player.kill() }, 7400)
}

function report() {
  const win0Changed = mediaChanged.includes(0)
  const win1Changed = mediaChanged.includes(1)
  const bothPlaying = playingWins.has(0) && playingWins.has(1)
  const pass = sawTwoWindows && win0Changed && win1Changed && bothPlaying &&
               sawOneWindowAfterDestroy && errors.length === 0
  console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  console.log(`창 2개 생성 관측: ${sawTwoWindows}`)
  console.log(`media_changed win0: ${win0Changed}, win1: ${win1Changed}`)
  console.log(`player_data is_playing 창: ${JSON.stringify([...playingWins])} (둘 다: ${bothPlaying})`)
  console.log(`destroy 후 창 1개: ${sawOneWindowAfterDestroy}`)
  console.log(`에러: ${errors.length}${errors.length ? ' ' + JSON.stringify(errors) : ''}`)
  process.exit(pass ? 0 : 1)
}
