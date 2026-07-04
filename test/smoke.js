#!/usr/bin/env node
// vplayer 스모크 테스트: spawn → stdout 포트 핸드셰이크 → TCP 접속 → 명령 주입 → 피드백 출력
//
// 사용법:
//   node test/smoke.js <vplayer.exe 경로> [재생할 미디어 파일 경로]
//
// 미디어 파일을 주면 playid → 5초 재생 → set_time(2s) → 3초 → stop 시나리오까지 수행.
// 안 주면 핸드셰이크 + get_audio_devices만 확인.

const { spawn } = require('child_process')
const net = require('net')

const exePath = process.argv[2]
const mediaPath = process.argv[3]

if (!exePath) {
  console.error('usage: node test/smoke.js <vplayer.exe> [media-file]')
  process.exit(1)
}

const player = spawn(exePath, [], { stdio: ['pipe', 'pipe', 'pipe'] })
let connected = false

player.stderr.on('data', (d) => process.stderr.write(`[stderr] ${d}`))
player.on('close', (code) => {
  console.log(`[player] exited with code ${code}`)
  process.exit(connected ? 0 : 1)
})

let stdoutBuf = ''
player.stdout.on('data', (data) => {
  stdoutBuf += data.toString()
  const nl = stdoutBuf.indexOf('\n')
  if (nl === -1) return
  const line = stdoutBuf.slice(0, nl).trim()
  console.log(`[handshake] ${line}`)
  const msg = JSON.parse(line)
  if (msg.type !== 'port') {
    console.error('FAIL: first stdout line is not a port handshake')
    player.kill()
    return
  }
  connect(msg.data.port)
})

function connect(port) {
  const sock = net.createConnection({ host: '127.0.0.1', port }, () => {
    connected = true
    console.log(`[tcp] connected on ${port}`)
    send(sock, { command: 'get_audio_devices' })
    if (mediaPath) runPlaybackScenario(sock)
    else setTimeout(() => finish(sock), 2000)
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
      // player_data 틱은 1초에 한 번만 출력 (로그 홍수 방지)
      if (msg.type === 'player_data') {
        if (!connect.lastTick || Date.now() - connect.lastTick > 1000) {
          connect.lastTick = Date.now()
          console.log(`[tick] t=${msg.data.time}ms / ${msg.data.duration}ms state=${msg.data.state}`)
        }
      } else {
        console.log(`[feedback] ${line}`)
      }
    }
  })
  sock.on('error', (e) => console.error(`[tcp] error: ${e.message}`))
}

function send(sock, obj) {
  const line = JSON.stringify(obj)
  console.log(`[send] ${line}`)
  sock.write(line + '\n')
}

function runPlaybackScenario(sock) {
  setTimeout(() => send(sock, { command: 'playid', file: { path: mediaPath, uuid: 'smoke-test' } }), 500)
  setTimeout(() => send(sock, { command: 'set_time', time: 2000 }), 5500)
  setTimeout(() => send(sock, { command: 'pause' }), 8500)
  setTimeout(() => send(sock, { command: 'play' }), 10000)
  setTimeout(() => send(sock, { command: 'stop' }), 12000)
  setTimeout(() => finish(sock), 13000)
}

function finish(sock) {
  console.log('[smoke] done — killing player')
  sock.destroy()
  player.kill()
}
