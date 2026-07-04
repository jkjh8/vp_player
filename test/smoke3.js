#!/usr/bin/env node
// vplayer 이미지 스틸/타이머 + 로고 오버레이 + 오디오 전용 스모크 테스트
//
// 사용법: node test/smoke3.js <vplayer.exe> <image.png> <logo.svg> <audio.m4a>
//
// 시나리오:
//   logo_file → show_logo(true) → logo_size(300)
//   → playid 이미지(time=3s) → 로고 숨김 + 3초 후 end_reached 검증
//   → playid 오디오 전용 → 로고 표시 검증
//   → stop → 로고 표시 (프로토콜 §2.7 필수)

const { spawn } = require('child_process')
const net = require('net')

const [exePath, imagePath, logoPath, audioPath] = process.argv.slice(2)
if (!exePath || !imagePath || !logoPath || !audioPath) {
  console.error('usage: node test/smoke3.js <vplayer.exe> <image.png> <logo.svg> <audio.m4a>')
  process.exit(1)
}

const player = spawn(exePath, [], { stdio: ['pipe', 'pipe', 'pipe'] })
const events = []
let imageStartedAt = 0
let endReachedDelay = -1

player.stderr.on('data', (d) => process.stderr.write(`[stderr] ${d}`))
player.on('close', () => {
  const logoSeq = events.filter((e) => e.type === 'logo_visibility').map((e) => e.data.show)
  const errors = events.filter((e) => e.type === 'error').map((e) => e.data)
  const endOk = endReachedDelay >= 2500 && endReachedDelay <= 4500
  // 기대 로고 순서: show_logo(true)→true, 이미지 재생→false, 오디오 재생→true, stop→true
  const logoOk = JSON.stringify(logoSeq) === JSON.stringify([true, false, true, true])
  const pass = endOk && logoOk && errors.length === 0
  console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  console.log(`image end_reached delay: ${endReachedDelay}ms (expect ~3000)`)
  console.log(`logo_visibility sequence: ${JSON.stringify(logoSeq)} (expect [true,false,true,true])`)
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
        if (Date.now() - lastTick > 900) {
          lastTick = Date.now()
          console.log(`[tick] deck=${msg.data.id} t=${msg.data.time}/${msg.data.duration}ms`)
        }
        continue
      }
      console.log(`[feedback] ${line}`)
      events.push(msg)
      if (msg.type === 'media_changed' && msg.data.uuid === 'img') imageStartedAt = Date.now()
      if (msg.type === 'end_reached' && imageStartedAt && endReachedDelay < 0) {
        endReachedDelay = Date.now() - imageStartedAt
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
  setTimeout(() => send(sock, { command: 'logo_file', file: logoPath }), 800)
  setTimeout(() => send(sock, { command: 'show_logo', show: true }), 1200)
  setTimeout(() => send(sock, { command: 'logo_size', size: 300 }), 1500)
  // 이미지 3초 표시
  setTimeout(() => send(sock, { command: 'playid', file: { path: imagePath, uuid: 'img', is_image: true, time: 3 } }), 2000)
  // 이미지 end_reached(≈5초 시점) 후 오디오 전용 재생
  setTimeout(() => send(sock, { command: 'playid', file: { path: audioPath, uuid: 'aud', is_image: false, mimetype: 'audio/mp4' } }), 6500)
  setTimeout(() => send(sock, { command: 'stop' }), 10000)
  setTimeout(() => { sock.destroy(); player.kill() }, 11500)
}
