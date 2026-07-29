#!/usr/bin/env node
// vplayer 전 트랙 프리롤 풀 스모크 (Phase 2 / 요구사항 #4)
//
// 사용법: node test/smoke_preload.js <vplayer.exe> <f1> <f2> <f3>
//
// 시나리오:
//   1) set_preload_config {lookahead:2}
//   2) preload_playlist {window_id:0, tracks:[f1,f2,f3], current_index:0}
//      → 풀에 0,1,2 프리롤 (memory_status.pool_decks / prerolled_decks 증가)
//   3) play_current_and_load_next current=f1(idx0) → 풀에서 즉시 승격 (media_changed idx0)
//   4) next → 풀 승격 idx1, next → idx2
//   5) stop_all
// 검증: 프리롤 풀 관측, media_changed idx 0/1/2, 에러 0.

const { spawn } = require('child_process')
const net = require('net')

const [exePath, f1, f2, f3] = process.argv.slice(2)
if (!exePath || !f1 || !f2 || !f3) {
  console.error('usage: node test/smoke_preload.js <vplayer.exe> <f1> <f2> <f3>')
  process.exit(1)
}

const errors = []
const changedIdx = []
let maxPool = 0
let maxPreroll = 0

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
      const line = buf.slice(0, nl); buf = buf.slice(nl + 1)
      if (!line.trim()) continue
      const msg = JSON.parse(line)
      switch (msg.type) {
        case 'memory_status':
          maxPool = Math.max(maxPool, msg.data.pool_decks || 0)
          maxPreroll = Math.max(maxPreroll, msg.data.prerolled_decks || 0)
          break
        case 'media_changed':
          changedIdx.push(msg.data.playlist_track_index)
          console.log(`[media_changed] win=${msg.data.window_id} track=${msg.data.playlist_track_index} uuid=${msg.data.uuid}`)
          break
        case 'error': errors.push(msg.data); console.log(`[ERROR] ${msg.data}`); break
        case 'debug': if (/pool|preload|cap/.test(msg.data)) console.log(`[debug] ${msg.data}`); break
        default: break
      }
    }
  })
  sock.on('error', () => {})
}

function send(sock, obj) { console.log(`[send] ${JSON.stringify(obj).slice(0, 110)}`); sock.write(JSON.stringify(obj) + '\n') }

function run(sock) {
  const tracks = [
    { path: f1, uuid: 'T0', time: 0 },
    { path: f2, uuid: 'T1', time: 0 },
    { path: f3, uuid: 'T2', time: 0 },
  ]
  setTimeout(() => send(sock, { command: 'set_preload_config', lookahead: 2, max_decks: 8 }), 400)
  setTimeout(() => send(sock, { command: 'preload_playlist', window_id: 0, current_index: 0, tracks }), 700)
  setTimeout(() => send(sock, { command: 'play_current_and_load_next', window_id: 0, track_idx: 0, current: tracks[0] }), 3000)
  setTimeout(() => send(sock, { command: 'next', window_id: 0 }), 5000)
  setTimeout(() => send(sock, { command: 'next', window_id: 0 }), 7000)
  setTimeout(() => send(sock, { command: 'stop_all' }), 8600)
  setTimeout(() => { sock.destroy(); player.kill() }, 9400)
}

function report() {
  const has = (i) => changedIdx.includes(i)
  const poolWarmed = maxPool >= 2 || maxPreroll >= 2
  const pass = poolWarmed && has(0) && has(1) && has(2) && errors.length === 0
  console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  console.log(`풀 프리롤 관측: pool_decks 최대=${maxPool}, prerolled 최대=${maxPreroll} (>=2: ${poolWarmed})`)
  console.log(`media_changed 트랙 순서: ${JSON.stringify(changedIdx)} (0,1,2 모두: ${has(0) && has(1) && has(2)})`)
  console.log(`에러: ${errors.length}${errors.length ? ' ' + JSON.stringify(errors) : ''}`)
  process.exit(pass ? 0 : 1)
}
