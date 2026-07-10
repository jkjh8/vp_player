#!/usr/bin/env node
// vplayer Phase A-1 검증: N채널 오디오 버스 + channel_map 라우팅 + 라이브 디바이스 전환
// 사용법: node test/smoke7.js <vplayer.exe> <video>
//
// 검증 항목:
//   1. capabilities 피드백 (channel_map 포함)
//   2. get_audio_device_caps → devices[{deviceId,name,type,channels}]
//   3. v1 회귀: channel_map 없는 재생 (2ch 기본 버스)
//   4. (ASIO 연결 시) set_audio_device asio → "Nch bus" 적용 + 재생 유지
//   5. channel_map 지정 재생 (마지막 2채널로 라우팅) — 에러 0, 틱 진행
//   6. 재생 중 기본 디바이스 복귀 (버스 축소 재협상)

const { spawn } = require('child_process')
const net = require('net')

const [exePath, video] = process.argv.slice(2)
if (!exePath || !video) {
  console.error('usage: node smoke7.js <vplayer.exe> <video>')
  process.exit(2)
}

const player = spawn(exePath, [], { stdio: ['pipe', 'pipe', 'pipe'] })
const results = {}
const errors = [] // error 타입 피드백 수집
let capabilities = null
let lastInfo = ''
let ticks = []
let mediaChanged = []

player.stderr.on('data', () => {})
player.on('close', () => report())

let stdoutBuf = ''
player.stdout.on('data', (data) => {
  stdoutBuf += data.toString()
  const nl = stdoutBuf.indexOf('\n')
  if (nl === -1) return
  const msg = JSON.parse(stdoutBuf.slice(0, nl).trim())
  if (msg.type === 'port') connect(msg.data.port)
})

let sock
const waiters = [] // {pred, resolve}
function feed(msg) {
  if (msg.type === 'error') errors.push(String(msg.data))
  if (msg.type === 'capabilities') capabilities = msg.data
  if (msg.type === 'info') lastInfo = String(msg.data)
  if (msg.type === 'player_data') ticks.push(msg.data)
  if (msg.type === 'media_changed') mediaChanged.push(msg.data)
  for (let i = waiters.length - 1; i >= 0; i--) {
    if (waiters[i].pred(msg)) waiters.splice(i, 1)[0].resolve(msg)
  }
}

function waitFor(pred, timeoutMs, label) {
  return new Promise((resolve) => {
    const t = setTimeout(() => {
      const i = waiters.findIndex((w) => w.resolve === resolve)
      if (i >= 0) waiters.splice(i, 1)
      resolve(null)
    }, timeoutMs)
    waiters.push({
      pred,
      resolve: (m) => {
        clearTimeout(t)
        resolve(m)
      },
    })
  })
}

function send(cmd) {
  sock.write(JSON.stringify(cmd) + '\n')
}

function connect(port) {
  sock = net.createConnection({ host: '127.0.0.1', port }, () => run())
  let buf = ''
  sock.on('data', (data) => {
    buf += data.toString()
    let nl
    while ((nl = buf.indexOf('\n')) !== -1) {
      const line = buf.slice(0, nl)
      buf = buf.slice(nl + 1)
      if (!line.trim()) continue
      try {
        feed(JSON.parse(line))
      } catch {}
    }
  })
  sock.on('error', () => {})
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

// wait_ms 동안의 틱 진행 확인 (time 증가 + is_playing)
async function collectTicks(waitMs) {
  const before = ticks.length
  await sleep(waitMs)
  const got = ticks.slice(before)
  const playing = got.filter((t) => t.is_playing)
  const advancing =
    playing.length >= 2 && playing[playing.length - 1].time > playing[0].time
  return { count: got.length, advancing }
}

async function run() {
  await waitFor((m) => m.type === 'info' && String(m.data).includes('ready'), 5000, 'ready')

  // 1) capabilities + device caps
  results.capabilities = capabilities
  send({ command: 'get_audio_device_caps' })
  const caps = await waitFor((m) => m.type === 'audio_device_caps', 5000)
  results.deviceCaps = caps ? caps.data.devices : null
  const asio = (results.deviceCaps || []).find((d) => d.type === 'asio')

  // 2) v1 회귀: 기본 재생 (channel_map 없음)
  send({ command: 'playid', file: { path: video, uuid: 'smoke7-a' } })
  await waitFor((m) => m.type === 'media_changed', 10000)
  results.v1Play = await collectTicks(2000)

  // 3) ASIO 전환 (연결 시) — 재생 중 버스 확장 2ch → Nch
  if (asio) {
    errors.length = 0
    send({ command: 'set_audio_device', device_id: asio.deviceId })
    const applied = await waitFor(
      (m) => m.type === 'info' && String(m.data).includes('audio device applied'),
      8000,
    )
    results.asioApplied = applied ? applied.data : null
    results.asioBusCh = applied && /\((\d+)ch bus\)/.exec(applied.data)?.[1]
    results.asioTicks = await collectTicks(2000)
    results.asioErrors = errors.slice()

    // 4) channel_map 재생: 스테레오 → ASIO 마지막 2채널
    const map = [asio.channels - 2, asio.channels - 1]
    send({ command: 'playid', file: { path: video, uuid: 'smoke7-b', channel_map: map, volume: 80 } })
    await waitFor((m) => m.type === 'media_changed' && m.data.uuid === 'smoke7-b', 10000)
    results.mapPlay = await collectTicks(2000)
    results.mapErrors = errors.slice(results.asioErrors.length)

    // 5) 재생 중 기본 디바이스 복귀 (버스 축소)
    send({ command: 'set_audio_device', device_id: '' })
    const back = await waitFor(
      (m) => m.type === 'info' && String(m.data).includes('audio device applied'),
      8000,
    )
    results.backApplied = back ? back.data : null
    results.backTicks = await collectTicks(2000)
  } else {
    // ASIO 미연결: 기본 디바이스에서 channel_map [0,1] (항등)만 검증
    send({ command: 'playid', file: { path: video, uuid: 'smoke7-b', channel_map: [0, 1] } })
    await waitFor((m) => m.type === 'media_changed' && m.data.uuid === 'smoke7-b', 10000)
    results.mapPlay = await collectTicks(2000)
  }

  send({ command: 'stop_all' })
  await sleep(300)
  results.totalErrors = errors.slice()
  sock.destroy()
  player.kill()
}

function report() {
  console.log(JSON.stringify(results, null, 2))
  const capOk =
    results.capabilities && (results.capabilities.features || []).includes('channel_map')
  const devOk = Array.isArray(results.deviceCaps) && results.deviceCaps.length > 0
  const v1Ok = results.v1Play && results.v1Play.advancing
  const mapOk = results.mapPlay && results.mapPlay.advancing
  const asioTested = 'asioApplied' in results
  const asioOk =
    !asioTested ||
    (results.asioApplied &&
      results.asioTicks.advancing &&
      results.asioErrors.length === 0 &&
      results.mapErrors.length === 0 &&
      results.backApplied &&
      results.backTicks.advancing)
  const noErrors = (results.totalErrors || []).length === 0
  const pass = capOk && devOk && v1Ok && mapOk && asioOk && noErrors
  console.log(`\ncapabilities: ${capOk ? 'OK' : 'NG'}, deviceCaps: ${devOk ? 'OK' : 'NG'}, ` +
    `v1 play: ${v1Ok ? 'OK' : 'NG'}, map play: ${mapOk ? 'OK' : 'NG'}, ` +
    `asio roundtrip: ${asioTested ? (asioOk ? 'OK' : 'NG') : 'skipped'}, errors: ${noErrors ? 0 : results.totalErrors.length}`)
  console.log(`=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  process.exit(pass ? 0 : 1)
}
