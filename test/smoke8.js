#!/usr/bin/env node
// vplayer Phase A-2 검증: 독립 오디오 트랙 (audio_track_* v2 §5)
// 사용법: node test/smoke8.js <vplayer.exe> <video> <loopWav(~3s)> <oneshotWav(~4s)>
//
// 검증 항목:
//   1. 비디오(덱) 재생 중 오디오 트랙 2개 병행 재생 — 덱/트랙 틱 동시 수신
//   2. loop=true 트랙: 파일 길이를 넘겨도 계속 재생 (time이 리셋되며 순환)
//   3. loop=false 트랙: 자연 종료 시 state:"stopped" 1회 후 틱 중단
//   4. audio_track_pause 토글 / set_volume / set_channel_map — 에러 0
//   5. audio_track_stop → stopped 피드백, 이후 틱 없음

const { spawn } = require('child_process')
const net = require('net')

const [exePath, video, loopWav, oneshotWav] = process.argv.slice(2)
if (!exePath || !video || !loopWav || !oneshotWav) {
  console.error('usage: node smoke8.js <vplayer.exe> <video> <loopWav> <oneshotWav>')
  process.exit(2)
}

const player = spawn(exePath, [], { stdio: ['pipe', 'pipe', 'pipe'] })
const results = {}
const errors = []
const trackTicks = { A: [], B: [] } // audio_track_data 누적
let deckTicks = 0

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
function feed(msg) {
  if (msg.type === 'error') errors.push(String(msg.data))
  if (msg.type === 'player_data') deckTicks++
  if (msg.type === 'audio_track_data' && trackTicks[msg.data.track_id]) {
    trackTicks[msg.data.track_id].push({ ...msg.data, at: Date.now() })
  }
  for (let i = waiters.length - 1; i >= 0; i--) {
    if (waiters[i].pred(msg)) waiters.splice(i, 1)[0].resolve(msg)
  }
}

function waitFor(pred, timeoutMs) {
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

let sock
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

async function run() {
  await waitFor((m) => m.type === 'info' && String(m.data).includes('ready'), 5000)

  // 1) 덱 재생 (병행의 배경)
  send({ command: 'playid', file: { path: video, uuid: 'smoke8-video' } })
  await waitFor((m) => m.type === 'media_changed', 10000)

  // 2) 오디오 트랙 2개: A = 3s 루프 (모노→버스 ch0), B = 4s 단발 (스테레오)
  send({ command: 'audio_track_play', track_id: 'A', file: { path: loopWav }, loop: true, volume: 50, channel_map: [0] })
  send({ command: 'audio_track_play', track_id: 'B', file: { path: oneshotWav }, volume: 40 })
  await waitFor((m) => m.type === 'debug' && String(m.data).includes('audio track started: A'), 8000)
  await waitFor((m) => m.type === 'debug' && String(m.data).includes('audio track started: B'), 8000)

  // 3) 병행 관찰 2s: 덱 + A + B 모두 틱
  const deckBefore = deckTicks
  const aBefore = trackTicks.A.length
  const bBefore = trackTicks.B.length
  await sleep(2000)
  results.parallel = {
    deck: deckTicks - deckBefore,
    trackA: trackTicks.A.length - aBefore,
    trackB: trackTicks.B.length - bBefore,
  }

  // 4) B 자연 종료 대기 (4s 파일 — 총 ~2.5s 경과, 추가 3.5s면 종료)
  const bStopped = await waitFor(
    (m) => m.type === 'audio_track_data' && m.data.track_id === 'B' && m.data.state === 'stopped',
    6000,
  )
  results.oneshotStopped = !!bStopped
  const bCountAtStop = trackTicks.B.length
  // 5) A 루프 확인: 지금까지 총 ~6s 경과 (3s 파일) — 여전히 재생 중 + time이 파일 길이 내
  await sleep(1500)
  const aRecent = trackTicks.A.slice(-10)
  results.loopStillPlaying = aRecent.length > 0 && aRecent.every((t) => t.state !== 'stopped')
  results.loopTimeBounded = aRecent.every((t) => t.time < 3600)
  results.loopWrapped = trackTicks.A.some((t, i) => i > 0 && t.time < trackTicks.A[i - 1].time - 500)
  results.oneshotNoTicksAfterStop = trackTicks.B.length === bCountAtStop

  // 6) pause 토글
  send({ command: 'audio_track_pause', track_id: 'A' })
  const pausedTick = await waitFor(
    (m) => m.type === 'audio_track_data' && m.data.track_id === 'A' && m.data.state === 'paused',
    3000,
  )
  results.paused = !!pausedTick
  await sleep(500)
  send({ command: 'audio_track_pause', track_id: 'A' })
  const resumed = await waitFor(
    (m) => m.type === 'audio_track_data' && m.data.track_id === 'A' && m.data.is_playing === true,
    3000,
  )
  results.resumed = !!resumed

  // 7) 볼륨/맵 라이브 변경
  send({ command: 'audio_track_set_volume', track_id: 'A', volume: 20 })
  send({ command: 'audio_track_set_channel_map', track_id: 'A', map: [1] })
  await sleep(500)

  // 8) 정지
  send({ command: 'audio_track_stop', track_id: 'A' })
  const aStopped = await waitFor(
    (m) => m.type === 'audio_track_data' && m.data.track_id === 'A' && m.data.state === 'stopped',
    3000,
  )
  results.stopFeedback = !!aStopped
  const aCountAtStop = trackTicks.A.length
  await sleep(700)
  results.noTicksAfterStop = trackTicks.A.length === aCountAtStop

  send({ command: 'stop_all' })
  await sleep(300)
  results.errors = errors.slice()
  sock.destroy()
  player.kill()
}

function report() {
  console.log(JSON.stringify(results, null, 2))
  const p = results.parallel || {}
  const parallelOk = p.deck >= 10 && p.trackA >= 10 && p.trackB >= 10
  const pass =
    parallelOk &&
    results.oneshotStopped &&
    results.oneshotNoTicksAfterStop &&
    results.loopStillPlaying &&
    results.loopTimeBounded &&
    results.loopWrapped &&
    results.paused &&
    results.resumed &&
    results.stopFeedback &&
    results.noTicksAfterStop &&
    (results.errors || []).length === 0
  console.log(
    `\nparallel: ${parallelOk ? 'OK' : 'NG'}, oneshot end: ${results.oneshotStopped ? 'OK' : 'NG'}, ` +
      `loop: ${results.loopStillPlaying && results.loopWrapped ? 'OK' : 'NG'}, ` +
      `pause/resume: ${results.paused && results.resumed ? 'OK' : 'NG'}, ` +
      `stop: ${results.stopFeedback && results.noTicksAfterStop ? 'OK' : 'NG'}, errors: ${(results.errors || []).length}`,
  )
  console.log(`=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  process.exit(pass ? 0 : 1)
}
