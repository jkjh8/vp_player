#!/usr/bin/env node
// vplayer ASIO 출력 검증: 재생 → ASIO 디바이스로 라이브 전환 → 틱 지속/무에러 확인
//
// 사용법: node test/smoke5.js <vplayer.exe> <media>
// ASIO 디바이스(type:asio)가 열람되지 않으면 SKIP.

const { spawn } = require('child_process')
const net = require('net')

const [exePath, media] = process.argv.slice(2)
const player = spawn(exePath, [], { stdio: ['pipe', 'pipe', 'pipe'] })

let asioId = null
let asioChannels = 0
let switched = false
let ticksAfter = 0
const errorsAfter = []
let infoApplied = false

player.stderr.on('data', (d) => process.stderr.write(`[stderr] ${d}`))
player.on('close', () => {
  if (!asioId) {
    console.log('\n=== RESULT: SKIP (no ASIO device enumerated) ===')
    process.exit(0)
  }
  const pass = infoApplied && ticksAfter > 5 && errorsAfter.length === 0
  console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  console.log(`asio device: ${asioId} (channels=${asioChannels})`)
  console.log(`applied=${infoApplied}, ticks after switch=${ticksAfter}, errors after=${errorsAfter.length}`)
  if (errorsAfter.length) console.log(`errors: ${JSON.stringify(errorsAfter)}`)
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
      if (msg.type === 'player_data') {
        if (switched) ticksAfter++
        if (Date.now() - lastTick > 1500) { lastTick = Date.now(); console.log(`[tick] t=${msg.data.time}ms`) }
        continue
      }
      console.log(`[feedback] ${line.slice(0, 160)}`)
      if (msg.type === 'audiodevices') {
        const a = msg.data.devices.find((d) => d.type === 'asio')
        if (a) { asioId = a.deviceId; asioChannels = a.channels || 0 }
      }
      if (msg.type === 'info' && String(msg.data).includes('audio device applied')) infoApplied = true
      if (msg.type === 'error' && switched) errorsAfter.push(msg.data)
    }
  })
  sock.on('error', () => {})
}

function send(sock, obj) { console.log(`[send] ${JSON.stringify(obj)}`); sock.write(JSON.stringify(obj) + '\n') }

function run(sock) {
  setTimeout(() => send(sock, { command: 'get_audio_devices' }), 500)
  setTimeout(() => send(sock, { command: 'playid', file: { path: media, uuid: 'asio-test' } }), 1500)
  setTimeout(() => {
    if (asioId) { send(sock, { command: 'set_audio_device', device_id: asioId }); switched = true }
  }, 4000)
  setTimeout(() => send(sock, { command: 'stop' }), 9000)
  setTimeout(() => { sock.destroy(); player.kill() }, 10000)
}
