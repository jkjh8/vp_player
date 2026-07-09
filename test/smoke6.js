#!/usr/bin/env node
// vplayer probe_media / make_thumbnail 검증 (Phase 2.5 — ffmpeg 대체)
// 사용법: node test/smoke6.js <vplayer.exe> <video> <image> <outdir>

const { spawn } = require('child_process')
const net = require('net')
const fs = require('fs')
const path = require('path')

const [exePath, video, image, outdir] = process.argv.slice(2)
const player = spawn(exePath, [], { stdio: ['pipe', 'pipe', 'pipe'] })
const results = {}
let reqSeq = 0
const pending = {}

player.stderr.on('data', () => {})
player.on('close', () => report())

let stdoutBuf = ''
player.stdout.on('data', (data) => {
  stdoutBuf += data.toString()
  const nl = stdoutBuf.indexOf('\n'); if (nl === -1) return
  const msg = JSON.parse(stdoutBuf.slice(0, nl).trim())
  if (msg.type === 'port') connect(msg.data.port)
})

let sock
function connect(port) {
  sock = net.createConnection({ host: '127.0.0.1', port }, () => run())
  let buf = ''
  sock.on('data', (data) => {
    buf += data.toString()
    let nl
    while ((nl = buf.indexOf('\n')) !== -1) {
      const line = buf.slice(0, nl); buf = buf.slice(nl + 1)
      if (!line.trim()) continue
      const msg = JSON.parse(line)
      if (msg.type === 'probe_result' || msg.type === 'thumbnail_result') {
        const p = pending[msg.data.req_id]; if (p) { delete pending[msg.data.req_id]; p(msg.data) }
      }
    }
  })
  sock.on('error', () => {})
}

function req(command, args) {
  return new Promise((resolve) => {
    const req_id = ++reqSeq
    pending[req_id] = resolve
    sock.write(JSON.stringify({ command, req_id, ...args }) + '\n')
    setTimeout(() => { if (pending[req_id]) { delete pending[req_id]; resolve({ req_id, ok: false, error: 'timeout' }) } }, 25000)
  })
}

async function run() {
  await new Promise((r) => setTimeout(r, 1500)) // ready 대기
  // 1) 비디오 프로브
  results.probeVideo = await req('probe_media', { path: video })
  // 2) 이미지 프로브
  results.probeImage = await req('probe_media', { path: image })
  // 3) 비디오 썸네일
  const vthumb = path.join(outdir, 'thumb-video.png')
  results.thumbVideo = await req('make_thumbnail', { path: video, out: vthumb, is_image: false, at_sec: 5, width: 320 })
  results.thumbVideo.exists = fs.existsSync(vthumb)
  results.thumbVideo.bytes = results.thumbVideo.exists ? fs.statSync(vthumb).size : 0
  // 4) 이미지 썸네일(리사이즈)
  const ithumb = path.join(outdir, 'thumb-image.png')
  results.thumbImage = await req('make_thumbnail', { path: image, out: ithumb, is_image: true, at_sec: 0, width: 320 })
  results.thumbImage.exists = fs.existsSync(ithumb)
  results.thumbImage.bytes = results.thumbImage.exists ? fs.statSync(ithumb).size : 0
  sock.destroy(); player.kill()
}

function report() {
  console.log(JSON.stringify(results, null, 2))
  const pv = results.probeVideo || {}
  const vstream = (pv.streams || []).find((s) => s.codec_type === 'video')
  const astream = (pv.streams || []).find((s) => s.codec_type === 'audio')
  const pass =
    pv.ok && vstream && vstream.width > 0 && astream && astream.channels > 0 &&
    (results.probeImage || {}).ok &&
    results.thumbVideo.ok && results.thumbVideo.bytes > 0 &&
    results.thumbImage.ok && results.thumbImage.bytes > 0
  console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`)
  if (vstream) console.log(`video: ${vstream.codec_name} ${vstream.width}x${vstream.height}, audio: ${astream ? astream.codec_name + ' ' + astream.channels + 'ch' : 'none'}`)
  console.log(`video thumb: ${results.thumbVideo.bytes}B, image thumb: ${results.thumbImage.bytes}B`)
  process.exit(pass ? 0 : 1)
}
