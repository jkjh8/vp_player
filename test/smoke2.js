#!/usr/bin/env node
// vplayer A/B 갭리스 스왑 스모크 테스트
//
// 사용법: node test/smoke2.js <vplayer.exe> <fileA> <fileB>
//
// 시나리오: playid A → 3초 → preload_next B → 5.5초 시점 next → B 재생 확인 → stop
// 검증 포인트: active_player_id 0→1 교대, media_changed B, next 이후 틱이 B의 0부터 진행

const { spawn } = require('child_process')
const net = require('net')

const [exePath, fileA, fileB] = process.argv.slice(2)
if (!exePath || !fileA || !fileB) {
  console.error('usage: node test/smoke2.js <vplayer.exe> <fileA> <fileB>')
  process.exit(1)
}

const player = spawn(exePath, [], { stdio: ['pipe', 'pipe', 'pipe'] })
const events = []  // 검증용 이산 이벤트 기록

player.stderr.on('data', (d) => process.stderr.write(`[stderr] ${d}`))
player.on('close', () => {
  // 최종 검증
  const ids = events.filter((e) => e.type === 'active_player_id').map((e) => e.data)
  const changed = events.filter((e) => e.type === 'media_changed').map((e) => e.data.uuid)
  const errors = events.filter((e) => e.type === 'error')
  const pass =
    ids.length >= 2 && ids[0] !== ids[1] &&
    changed.length >= 2 && changed[0] === 'file-A' && changed[1] === 'file-B' &&
    errors.length === 0
  console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  console.log(`active_player_id sequence: ${JSON.stringify(ids)}`)
  console.log(`media_changed sequence:    ${JSON.stringify(changed)}`)
  if (errors.length) console.log(`errors: ${JSON.stringify(errors.map((e) => e.data))}`)
  process.exit(pass ? 0 : 1)
})

let stdoutBuf = ''
player.stdout.on('data', (data) => {
  stdoutBuf += data.toString()
  const nl = stdoutBuf.indexOf('\n')
  if (nl === -1) return
  const msg = JSON.parse(stdoutBuf.slice(0, nl).trim())
  if (msg.type === 'port') connect(msg.data.port)
})

let lastTick = 0
function connect(port) {
  const sock = net.createConnection({ host: '127.0.0.1', port }, () => {
    console.log(`[tcp] connected on ${port}`)
    run(sock)
  })
  let buf = ''
  sock.on('data', (data) => {
    buf += data.toString()
    let nl
    while ((nl = buf.indexOf('\n')) !== -1) {
      const line = buf.slice(0, nl)
      buf = buf.slice(nl + 1)
      if (!line.trim()) continue
      const msg = JSON.parse(line)
      if (msg.type === 'player_data') {
        if (Date.now() - lastTick > 900) {
          lastTick = Date.now()
          console.log(`[tick] deck=${msg.data.id} t=${msg.data.time}ms / ${msg.data.duration}ms`)
        }
      } else {
        console.log(`[feedback] ${line}`)
        if (['active_player_id', 'media_changed', 'error', 'end_reached'].includes(msg.type)) {
          events.push(msg)
        }
      }
    }
  })
  sock.on('error', () => {})
}

function send(sock, obj) {
  console.log(`[send] ${JSON.stringify(obj)}`)
  sock.write(JSON.stringify(obj) + '\n')
}

function run(sock) {
  setTimeout(() => send(sock, { command: 'playid', file: { path: fileA, uuid: 'file-A' } }), 500)
  setTimeout(() => send(sock, { command: 'preload_next', next: { path: fileB, uuid: 'file-B' }, next_track_idx: 1 }), 3500)
  setTimeout(() => send(sock, { command: 'next' }), 6000)
  setTimeout(() => send(sock, { command: 'stop' }), 10000)
  setTimeout(() => { sock.destroy(); player.kill() }, 11000)
}
