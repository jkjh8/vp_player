#!/usr/bin/env node
// vplayer 타임라인 인포인트(in_ms) 프리롤 스파이크 — B1a 위험 선검증
//
// 사용법: node test/smoke10.js <vplayer.exe> <video.mp4 (>=20s)>
//
// 검증: playid 의 file 객체에 옵션 in_ms 를 실으면, 덱이 위치 0에서 프리롤 완료 후
// FLUSH|ACCURATE 시크로 해당 지점에 재배치되어 재생이 in_ms 부터 시작해야 한다.
//   1) in_ms=15000 으로 재생 → 초기 player_data 틱의 time 이 ~15000ms 근방(≥14000)에서 시작
//   2) 시간이 단조 증가(재생 진행)
//   3) 에러 피드백 0

const { spawn } = require('child_process')
const net = require('net')

const [exePath, videoPath] = process.argv.slice(2)
if (!exePath || !videoPath) {
  console.error('usage: node test/smoke10.js <vplayer.exe> <video.mp4 (>=20s)>')
  process.exit(1)
}

const IN_MS = 15000
const ticks = []
const errors = []

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
      if (msg.type === 'player_data') {
        ticks.push(msg.data.time)
        if (Date.now() - lastLog > 700) {
          lastLog = Date.now()
          console.log(`[tick] t=${msg.data.time}/${msg.data.duration}ms`)
        }
        continue
      }
      console.log(`[feedback] ${line}`)
      if (msg.type === 'error') errors.push(msg.data)
    }
  })
  sock.on('error', () => {})
}

function send(sock, obj) {
  console.log(`[send] ${JSON.stringify(obj)}`)
  sock.write(JSON.stringify(obj) + '\n')
}

function run(sock) {
  // in_ms=15000 으로 재생 시작
  setTimeout(() => send(sock, {
    command: 'playid',
    file: { path: videoPath, uuid: 'clip', is_image: false, in_ms: IN_MS }
  }), 800)
  // ~4초 재생 후 종료
  setTimeout(() => send(sock, { command: 'stop' }), 5500)
  setTimeout(() => { sock.destroy(); player.kill() }, 6500)
}

function report() {
  // 스왑 직후 안정화된 틱만 (첫 2틱은 협상 흔들림 배제)
  const stable = ticks.slice(2).filter((t) => t > 0)
  const first = stable.length ? stable[0] : -1
  const last = stable.length ? stable[stable.length - 1] : -1
  const startedAtInpoint = first >= 14000 && first <= 18000
  const climbing = last > first
  const pass = startedAtInpoint && climbing && errors.length === 0
  console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  console.log(`in_ms 요청: ${IN_MS}ms`)
  console.log(`안정 틱 수: ${stable.length}, 첫 틱: ${first}ms (기대 ~15000, [14000,18000]), 마지막 틱: ${last}ms`)
  console.log(`인포인트 시작: ${startedAtInpoint}, 단조증가: ${climbing}, 에러: ${errors.length}`)
  if (errors.length) console.log(`errors: ${JSON.stringify(errors)}`)
  process.exit(pass ? 0 : 1)
}
