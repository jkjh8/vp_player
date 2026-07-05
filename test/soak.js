#!/usr/bin/env node
// vplayer 소크 테스트: 트랙을 빠르게 순환(덱 생성/해체 반복)하며 메모리 추이 관찰.
// 덱 lifecycle이 누수의 주 표면 — playid를 교대로 반복해 최대한 스트레스.
//
// 사용법: node test/soak.js <vplayer.exe> <video> <image> [durationSec=300] [intervalMs=2000]

const { spawn } = require('child_process')
const net = require('net')
const { execSync } = require('child_process')

const [exePath, video, image, durSec = '300', intMs = '2000'] = process.argv.slice(2)
const durationMs = parseInt(durSec) * 1000
const interval = parseInt(intMs)

const player = spawn(exePath, [], { stdio: ['pipe', 'pipe', 'pipe'] })
const pid = player.pid
const samples = []
let cycles = 0
let errors = 0

player.stderr.on('data', () => {})  // CRITICAL 경고 무시 (기존 무해 이슈)
player.on('close', () => report())

function rssMB(p) {
  try {
    const out = execSync(`powershell -NoProfile -Command "(Get-Process -Id ${p} -ErrorAction SilentlyContinue).WorkingSet64"`, { encoding: 'utf8' }).trim()
    return out ? Math.round(parseInt(out) / 1048576) : -1
  } catch { return -1 }
}

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
      if (msg.type === 'error') { errors++; console.log(`[error] ${msg.data}`) }
    }
  })
  sock.on('error', () => {})
}

function send(sock, obj) { sock.write(JSON.stringify(obj) + '\n') }

function run(sock) {
  const start = Date.now()
  // 짧은 이미지(2s) + 비디오 교대 재생, interval마다 강제 전환
  const tracks = [
    { path: video, uuid: 'v', is_image: false },
    { path: image, uuid: 'i', is_image: true, time: 2 },
  ]
  let idx = 0
  const tick = setInterval(() => {
    const elapsed = Date.now() - start
    if (elapsed >= durationMs) {
      clearInterval(tick)
      send(sock, { command: 'stop' })
      setTimeout(() => { sock.destroy(); player.kill() }, 500)
      return
    }
    const t = tracks[idx % tracks.length]
    send(sock, { command: 'playid', file: t, track_idx: idx })
    idx++; cycles++
    if (cycles % 5 === 0) {
      const mb = rssMB(pid)
      samples.push({ s: Math.round(elapsed / 1000), mb })
      console.log(`[${Math.round(elapsed / 1000)}s] cycle ${cycles}  RSS=${mb}MB  errors=${errors}`)
    }
  }, interval)
}

function report() {
  if (samples.length < 3) { console.log('\n=== SOAK: insufficient samples ==='); process.exit(1) }
  const first = samples[1].mb  // [0]은 워밍업이라 제외
  const last = samples[samples.length - 1].mb
  const growth = last - first
  const peakMB = Math.max(...samples.map((s) => s.mb))
  // 판정: 성장률이 초기의 25% 이내 & 에러 0이면 PASS (덱 순환당 소량 변동은 정상)
  const pass = errors === 0 && growth <= first * 0.25
  console.log(`\n=== SOAK RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  console.log(`cycles: ${cycles}, duration: ${durSec}s`)
  console.log(`RSS first=${first}MB last=${last}MB growth=${growth}MB peak=${peakMB}MB`)
  console.log(`errors: ${errors}`)
  process.exit(pass ? 0 : 1)
}
