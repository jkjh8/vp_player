#!/usr/bin/env node
// vplayer 레거시 트랙 경로(set_tracks/playlist_play/next폴백/previous) + set_audio_device 라이브 전환
//
// 사용법: node test/smoke4.js <vplayer.exe> <fileA> <fileB>

const { spawn } = require('child_process')
const net = require('net')

const [exePath, fileA, fileB] = process.argv.slice(2)
const player = spawn(exePath, [], { stdio: ['pipe', 'pipe', 'pipe'] })
const changed = []   // media_changed uuid 순서
const infos = []
const errors = []
let deviceId = null
let tickAfterSwitch = 0

player.stderr.on('data', (d) => process.stderr.write(`[stderr] ${d}`))
player.on('close', () => {
  // 기대: playlist_play→A, next(폴백)→B, previous→A / 디바이스 적용 info / 전환 후 틱 진행
  const seqOk = JSON.stringify(changed) === JSON.stringify(['trk-A', 'trk-B', 'trk-A'])
  const devOk = infos.some((t) => String(t).includes('audio device applied'))
  const pass = seqOk && devOk && tickAfterSwitch > 0 && errors.length === 0
  console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  console.log(`media_changed: ${JSON.stringify(changed)} (expect [trk-A,trk-B,trk-A])`)
  console.log(`device applied: ${devOk}, ticks after switch: ${tickAfterSwitch}`)
  if (errors.length) console.log(`errors: ${JSON.stringify(errors)}`)
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

let switched = false
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
        if (switched) tickAfterSwitch++
        if (Date.now() - lastTick > 1500) { lastTick = Date.now(); console.log(`[tick] deck=${msg.data.id} t=${msg.data.time}ms`) }
        continue
      }
      console.log(`[feedback] ${line}`)
      if (msg.type === 'media_changed') changed.push(msg.data.uuid)
      if (msg.type === 'info') infos.push(msg.data)
      if (msg.type === 'error') errors.push(msg.data)
      if (msg.type === 'audiodevices' && !deviceId) {
        const d = msg.data.devices.find((x) => x.deviceId.startsWith('{0.0.0')) || msg.data.devices[0]
        deviceId = d && d.deviceId
      }
    }
  })
  sock.on('error', () => {})
}

function send(sock, obj) {
  console.log(`[send] ${JSON.stringify(obj).slice(0, 100)}`)
  sock.write(JSON.stringify(obj) + '\n')
}

function run(sock) {
  const tracks = [
    { path: fileA, uuid: 'trk-A', is_image: false, time: 0 },
    { path: fileB, uuid: 'trk-B', is_image: false, time: 0 },
  ]
  setTimeout(() => send(sock, { command: 'get_audio_devices' }), 500)
  setTimeout(() => send(sock, { command: 'playlist_mode', value: true }), 800)
  setTimeout(() => send(sock, { command: 'set_tracks', tracks }), 1000)
  setTimeout(() => send(sock, { command: 'playlist_play', idx: 0 }), 1500)
  setTimeout(() => send(sock, { command: 'next' }), 5000)      // 프리로드 없음 → tracks_ 폴백
  setTimeout(() => send(sock, { command: 'previous' }), 8500)  // 1 → 0
  setTimeout(() => { if (deviceId) send(sock, { command: 'set_audio_device', device_id: deviceId }); switched = true }, 12000)
  setTimeout(() => send(sock, { command: 'stop' }), 15000)
  setTimeout(() => { sock.destroy(); player.kill() }, 16000)
}
