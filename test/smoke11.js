#!/usr/bin/env node
// vplayer 타임라인 모드 스모크 (v2 §5.2 — Phase B / B1b)
//
// 사용법: node test/smoke11.js <vplayer.exe> <videoA.mp4> <videoB.mp4> <audio.(m4a|wav)>
//
// 시나리오 (duration 20s):
//   비디오 트랙 V1(order 0, 최상위): clipA  0–6s  (in 0)
//   비디오 트랙 V2(order 1, 하위):   clipB  4–12s (in 0)   → 0–6 A, 6–12 B, 12–14 갭
//                                    clipB2 12→? 없음; 14–20 갭
//   오디오 트랙 A1(ch [0,1]):        audio  2–10s (in 0)
// 검증:
//   1) set_timeline → capabilities에 "timeline" 포함
//   2) timeline_play → timeline_position 틱 수신, time_ms 단조 증가
//   3) 세그먼트 전환(6s 근처 A→B) 무에러
//   4) timeline_seek(3000) → 위치가 ~3000으로 점프
//   5) timeline_pause → is_playing:false, timeline_play 재개
//   6) timeline_stop → is_playing:false, 에러 0

const { spawn } = require('child_process')
const net = require('net')

const [exePath, vaPath, vbPath, audPath] = process.argv.slice(2)
if (!exePath || !vaPath || !vbPath || !audPath) {
  console.error('usage: node test/smoke11.js <vplayer.exe> <videoA> <videoB> <audio>')
  process.exit(1)
}

const positions = []      // timeline_position time_ms 이력
const errors = []
let caps = null
let sawSeekJump = false
let pausedFalseSeen = false

const t0 = Date.now()
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

const timeline = {
  timeline_id: 'tl-smoke',
  duration_ms: 20000,
  tracks: [
    { track_id: 'v1', type: 'video', order: 0, mute: false, volume: 100, channel_map: null,
      clips: [ { clip_id: 'cA', file: { path: vaPath, uuid: 'A' }, start_ms: 0, in_ms: 0, out_ms: 6000, volume: 100 } ] },
    { track_id: 'v2', type: 'video', order: 1, mute: false, volume: 100, channel_map: null,
      clips: [ { clip_id: 'cB', file: { path: vbPath, uuid: 'B' }, start_ms: 4000, in_ms: 0, out_ms: 8000, volume: 100 } ] },
    { track_id: 'a1', type: 'audio', order: 0, mute: false, volume: 90, channel_map: [0, 1],
      clips: [ { clip_id: 'cAud', file: { path: audPath, uuid: 'AUD' }, start_ms: 2000, in_ms: 0, out_ms: 8000, volume: 100 } ] }
  ]
}

let lastLog = 0
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
      if (msg.type === 'capabilities') caps = msg.data.features
      if (msg.type === 'timeline_position') {
        positions.push({ t: msg.data.time_ms, playing: msg.data.is_playing, w: Date.now() - t0 })
        if (msg.data.is_playing === false) pausedFalseSeen = true
        if (Date.now() - lastLog > 600) {
          lastLog = Date.now()
          console.log(`[pos] t=${msg.data.time_ms}/${msg.data.duration_ms}ms playing=${msg.data.is_playing}`)
        }
        continue
      }
      if (msg.type === 'error') { errors.push(msg.data); console.log(`[ERROR] ${msg.data}`) }
      else console.log(`[feedback] ${line}`)
    }
  })
  sock.on('error', () => {})
}

function send(sock, obj) {
  const s = JSON.stringify(obj)
  console.log(`[send] ${s.length > 120 ? s.slice(0, 117) + '...' : s}`)
  sock.write(s + '\n')
}

let seekMark = -1
function run(sock) {
  setTimeout(() => send(sock, { command: 'set_timeline', ...timeline }), 700)
  setTimeout(() => send(sock, { command: 'timeline_play' }), 1200)
  // ~2.5s 재생(pos≈1500) 후 8000으로 전방 시크 → 명확한 점프
  setTimeout(() => { seekMark = positions.length; send(sock, { command: 'timeline_seek', time_ms: 8000 }) }, 3700)
  setTimeout(() => send(sock, { command: 'timeline_pause' }), 5400)
  setTimeout(() => send(sock, { command: 'timeline_play' }), 6600)
  setTimeout(() => send(sock, { command: 'timeline_stop' }), 8400)
  setTimeout(() => { sock.destroy(); player.kill() }, 9400)
}


function report() {
  // 시크(8000) 이후 위치 중 [7500,9500] 밴드 착지 여부
  if (seekMark >= 0) {
    sawSeekJump = positions.slice(seekMark).some((p) => p.t >= 7500 && p.t <= 9500)
  }
  const playing = positions.filter((p) => p.playing).map((p) => p.t)
  const climbing = playing.length >= 3 && playing[playing.length - 1] > playing[0]
  const capsOk = Array.isArray(caps) && caps.includes('timeline')
  const gotPositions = positions.length >= 5
  const pass = capsOk && gotPositions && climbing && sawSeekJump && pausedFalseSeen && errors.length === 0
  console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  console.log(`capabilities timeline: ${capsOk} (${JSON.stringify(caps)})`)
  console.log(`위치 틱 수: ${positions.length}, 재생 중 단조증가: ${climbing}`)
  console.log(`seek 점프 관측: ${sawSeekJump}, pause(is_playing:false) 관측: ${pausedFalseSeen}`)
  console.log(`위치 이력(t@wall playing): ${positions.map((p) => `${p.t}@${p.w}${p.playing ? '' : 'P'}`).join(' ')}`)
  console.log(`seekMark=${seekMark} (send seek at wall≈3700)`)
  console.log(`에러: ${errors.length}${errors.length ? ' ' + JSON.stringify(errors) : ''}`)
  process.exit(pass ? 0 : 1)
}
