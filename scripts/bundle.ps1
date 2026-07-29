# vplayer 배포 번들 생성: dist\player\ = vplayer.exe + 코어 DLL(의존성 클로저) + gst-plugins\
#
# 사용법:  powershell -File scripts\bundle.ps1 [-Config Release] [-OutDir dist]
# 결과:    dist\player\vplayer.exe (+ *.dll)  /  dist\player\gst-plugins\*.dll
#
# 플러그인 목록은 플랜의 "선별 플러그인 세트" — 실제 콘텐츠에서 재생 실패 시 여기에 추가.

param(
  [string]$Config = "Release",
  [string]$OutDir = "dist"
)
$ErrorActionPreference = "Stop"

$repo = Split-Path $PSScriptRoot -Parent
$gstRoot = $env:GSTREAMER_1_0_ROOT_MSVC_X86_64
if (-not $gstRoot) { $gstRoot = [System.Environment]::GetEnvironmentVariable('GSTREAMER_1_0_ROOT_MSVC_X86_64','Machine') }
if (-not $gstRoot) { $gstRoot = "C:\Program Files\gstreamer\1.0\msvc_x86_64" }
$gstBin = Join-Path $gstRoot "bin"
$gstPlugins = Join-Path $gstRoot "lib\gstreamer-1.0"

$exe = Join-Path $repo "build\$Config\vplayer.exe"
if (-not (Test-Path $exe)) { throw "vplayer.exe not found: $exe — cmake --build --preset $($Config.ToLower()) 먼저" }

$out = Join-Path $repo "$OutDir\player"
$outPlugins = Join-Path $out "gst-plugins"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $outPlugins | Out-Null

# --- 1. 선별 플러그인 세트 ------------------------------------------------------
$pluginNames = @(
  # 코어/재생 골격
  'coreelements','playback','typefindfunctions','app','autodetect',
  # 비디오 합성/변환 (d3d11 = HW 디코더+compositor+sink 포함, videocrop = aspectratiocrop)
  'compositor','d3d11','videoconvertscale','imagefreeze','videotestsrc','videocrop',
  # 오디오 경로
  'audioconvert','audioresample','audiomixer','volume','wasapi2','audiotestsrc',
  # 컨테이너/파서
  'isomp4','matroska','avi','wavparse','audioparsers','videoparsersbad','id3demux','ogg',
  # 코덱 (mediafoundation = OS HW/SW, libav = "뭐든 재생" 폴백)
  'mediafoundation','libav','jpeg','png','mpg123','opus','vorbis','flac'
)
$missing = @()
foreach ($name in $pluginNames) {
  $dll = Join-Path $gstPlugins "gst$name.dll"
  if (Test-Path $dll) { Copy-Item $dll $outPlugins } else { $missing += $name }
}
if ($missing) { Write-Warning "plugins not found (skipped): $($missing -join ', ')" }

# 직접 빌드한 asio 플러그인 (공식 바이너리 미포함 — third_party/gstasio, CMake 타깃)
$asioDll = Join-Path $repo "build\$Config\gstasio.dll"
if (Test-Path $asioDll) { Copy-Item $asioDll $outPlugins } else { Write-Warning "gstasio.dll not built — ASIO 미포함" }

# --- 2. exe 복사 ---------------------------------------------------------------
Copy-Item $exe $out

# --- 3. DLL 의존성 클로저 (dumpbin /dependents, GStreamer bin 안에서만 해석) -------
$dumpbin = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $dumpbin) { throw "dumpbin.exe not found (VS2022 C++ tools 필요)" }

$resolved = @{}   # 소문자 dll명 → 복사 완료
$queue = New-Object System.Collections.Queue
(Get-ChildItem $out -Filter *.dll -Recurse) + (Get-Item (Join-Path $out "vplayer.exe")) | ForEach-Object { $queue.Enqueue($_.FullName) }

while ($queue.Count -gt 0) {
  $file = $queue.Dequeue()
  $deps = & $dumpbin.FullName /nologo /dependents $file |
    Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim().ToLower() }
  foreach ($dep in $deps) {
    if ($resolved.ContainsKey($dep)) { continue }
    $src = Join-Path $gstBin $dep
    if (Test-Path $src) {
      $dst = Join-Path $out $dep
      if (-not (Test-Path $dst)) { Copy-Item $src $dst; $queue.Enqueue($dst) }
      $resolved[$dep] = $true
    } else {
      $resolved[$dep] = $false  # 시스템 DLL (kernel32, api-ms-* 등) — 스킵
    }
  }
}

# --- 4. MSVC 런타임 (앱 로컬 배치 — 대상 PC의 vc_redist 미설치 대비) ----------------
$crt = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($crt) {
  Copy-Item (Join-Path $crt.FullName "*.dll") $out
} else {
  Write-Warning "MSVC redist dir not found — vcruntime/msvcp DLL 미포함 (설치기에서 vc_redist 필요)"
}

# --- 5. 요약 --------------------------------------------------------------------
$coreCount = (Get-ChildItem $out -Filter *.dll -File).Count
$plugCount = (Get-ChildItem $outPlugins -Filter *.dll -File).Count
$sizeMB = [math]::Round(((Get-ChildItem $out -Recurse -File | Measure-Object Length -Sum).Sum) / 1MB, 1)
Write-Host ""
Write-Host "=== bundle complete: $out ==="
Write-Host "core DLLs: $coreCount, plugins: $plugCount, total size: $sizeMB MB"
