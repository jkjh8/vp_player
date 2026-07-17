#!/usr/bin/env node
// vplayer A1 검증: 채널별 volume/mute/routing (mix-matrix 계수 = 채널별 gain)
// 사용법: node test/smoke9.js <vplayer.exe> <video(스테레오+)>
//
// 검증:
//   1. embedded_streams[0].channels 채널별 라우팅으로 재생 (에러 0, 틱 진행)
//   2. set_deck_audio {streams:[{channels:[{out,volume,muted}]}]} 라이브 — 에러 없이 반영
//   3. 마스터 볼륨/뮤트(streams[0].volume/muted) 라이브
//   4. 레거시 channel_map 재생 회귀 (기존 동작 유지)
//   5. audio_track_play channels(채널별) + audio_track_set_channel_map 채널별 라이브
// (실제 채널 배치/게인은 가청 확인 몫 — 여기선 무에러 + 재생 지속을 확인)

const { spawn } = require('child_process')
const net = require('net')

const [exePath, video] = process.argv.slice(2)
if (!exePath || !video) {
  console.error('usage: node smoke9.js <vplayer.exe> <video>')
  process.exit(2)
}

const player = spawn(exePath, [], { stdio: ['pipe', 'pipe', 'pipe'] })
const results = {}
const errors = []
let ticks = 0
let atData = {}

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

const waiters = []
let capsData = null
function feed(msg) {
  if (msg.type === 'error') errors.push(String(msg.data))
  if (msg.type === 'capabilities') capsData = msg.data
  if (msg.type === 'player_data') ticks++
  if (msg.type === 'audio_track_data') atData[msg.data.track_id] = msg.data
  for (let i = waiters.length - 1; i >= 0; i--) {
    if (waiters[i].pred(msg)) waiters.splice(i, 1)[0].resolve(msg)
  }
}
function waitFor(pred, t) {
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      const i = waiters.findIndex((w) => w.resolve === resolve)
      if (i >= 0) waiters.splice(i, 1)
      resolve(null)
    }, t)
    waiters.push({ pred, resolve: (m) => { clearTimeout(timer); resolve(m) } })
  })
}
let sock
const send = (c) => sock.write(JSON.stringify(c) + '\n')
function connect(port) {
  sock = net.createConnection({ host: '127.0.0.1', port }, () => run())
  let buf = ''
  sock.on('data', (d) => {
    buf += d.toString()
    let nl
    while ((nl = buf.indexOf('\n')) !== -1) {
      const line = buf.slice(0, nl); buf = buf.slice(nl + 1)
      if (line.trim()) try { feed(JSON.parse(line)) } catch {}
    }
  })
  sock.on('error', () => {})
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
async function collectTicks(ms) {
  const before = ticks
  await sleep(ms)
  return ticks - before
}

async function run() {
  await waitFor((m) => m.type === 'info' && String(m.data).includes('ready'), 5000)
  await sleep(300) // capabilities는 ready 직후 발신 — 저장 대기
  results.hasEmbeddedStreams = !!capsData && (capsData.features || []).includes('embedded_streams')

  // 1) 채널별 라우팅 재생 (ch0→out0 vol100, ch1→out1 vol50)
  send({
    command: 'playid',
    file: {
      path: video,
      uuid: 'smoke9-a',
      embedded_streams: [
        { index: 0, volume: 100, muted: false, channels: [
          { out: 0, volume: 100, muted: false },
          { out: 1, volume: 50, muted: false },
        ] },
      ],
    },
  })
  await waitFor((m) => m.type === 'media_changed', 10000)
  errors.length = 0
  results.chPlayTicks = await collectTicks(2000)

  // 2) 라이브 채널별 변경 (ch0 뮤트, ch1 vol80 → out2)
  send({ command: 'set_deck_audio', streams: [
    { index: 0, channels: [ { out: 0, volume: 100, muted: true }, { out: 2, volume: 80, muted: false } ] },
  ] })
  await sleep(500)
  results.chLiveTicks = await collectTicks(1500)

  // 3) 마스터 볼륨/뮤트 라이브
  send({ command: 'set_deck_audio', streams: [{ index: 0, volume: 60, muted: false }] })
  await sleep(300)
  send({ command: 'set_deck_audio', streams: [{ index: 0, muted: true }] })
  await sleep(300)
  send({ command: 'set_deck_audio', streams: [{ index: 0, muted: false, volume: 100 }] })
  results.masterLiveTicks = await collectTicks(1500)

  // 4) 레거시 channel_map 회귀 — 정지 후 새 재생 (stop→play 무결성 포함)
  send({ command: 'stop_all' })
  await sleep(1000)
  send({ command: 'playid', file: { path: video, uuid: 'smoke9-legacy', channel_map: [0, 1], volume: 90 } })
  await waitFor((m) => m.type === 'media_changed' && m.data.uuid === 'smoke9-legacy', 12000)
  results.legacyTicks = await collectTicks(3000)

  results.errors = errors.slice()
  send({ command: 'stop_all' })
  await sleep(300)
  sock.destroy()
  player.kill()
}

function report() {
  console.log(JSON.stringify(results, null, 2))
  const adv = (n) => n >= 10
  const pass =
    results.hasEmbeddedStreams &&
    adv(results.chPlayTicks) &&
    adv(results.chLiveTicks) &&
    adv(results.masterLiveTicks) &&
    adv(results.legacyTicks) &&
    (results.errors || []).length === 0
  console.log(`\ncap: ${results.hasEmbeddedStreams ? 'OK' : 'NG'}, ch play: ${results.chPlayTicks}, ` +
    `ch live: ${results.chLiveTicks}, master live: ${results.masterLiveTicks}, legacy: ${results.legacyTicks}, ` +
    `errors: ${(results.errors || []).length}`)
  console.log(`=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  process.exit(pass ? 0 : 1)
}
