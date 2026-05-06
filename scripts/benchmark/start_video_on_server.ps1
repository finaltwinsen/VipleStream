# start_video_on_server.ps1 — SSH 到 server 啟動 PotPlayer 全螢幕、從 0:00 播指定影片
#
# 用法：
#   pwsh scripts\benchmark\start_video_on_server.ps1
#       -VideoPath "C:\Temp\videoplayback.webm"
#       [-ServerHost 192.168.51.226]
#
# 行為：kill 既有 PotPlayer → 啟動新 instance fullscreen + 從頭播放
# 用 Base64 EncodedCommand 通過 SSH 完整保留 PowerShell quoting

param(
    [string] $VideoPath  = "C:\Temp\videoplayback.webm",
    [string] $ServerHost = "192.168.51.226",
    [string] $User       = "final",
    [string] $PotPlayer  = "C:\Program Files\DAUM\PotPlayer\PotPlayerMini64.exe"
)

$ErrorActionPreference = "Stop"

# Server-side PowerShell script
$serverScript = @"
`$ErrorActionPreference = 'Stop'

# Kill existing PotPlayer if any
Get-Process PotPlayerMini64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

# Verify paths
if (-not (Test-Path '$PotPlayer')) {
    Write-Error 'PotPlayer not found at $PotPlayer'
    exit 1
}
if (-not (Test-Path '$VideoPath')) {
    Write-Error 'Video file not found at $VideoPath'
    exit 1
}

# Launch PotPlayer fullscreen from 0:00
# Args:
#   <file>           : auto-play
#   /fullscreen      : F11-equivalent fullscreen mode
#   /seek=0          : seek to start (0 sec)
#   /current_play    : play in this instance (vs queueing)
Start-Process -FilePath '$PotPlayer' -ArgumentList @('$VideoPath', '/fullscreen', '/current_play')

# Give PotPlayer a moment to render first frame
Start-Sleep -Seconds 2

# Verify it's running
if (Get-Process PotPlayerMini64 -ErrorAction SilentlyContinue) {
    Write-Output 'OK PotPlayer running fullscreen with video at 0:00'
    exit 0
} else {
    Write-Error 'PotPlayer launched but immediately exited'
    exit 1
}
"@

# Encode as UTF-16-LE Base64 (PowerShell -EncodedCommand expects this)
$bytes = [System.Text.Encoding]::Unicode.GetBytes($serverScript)
$encoded = [Convert]::ToBase64String($bytes)

Write-Host "[start-video] target: $User@$ServerHost" -ForegroundColor Cyan
Write-Host "[start-video] video: $VideoPath" -ForegroundColor Cyan

# SSH execute — Windows server's default shell is cmd.exe; powershell -EncodedCommand works
& ssh -o BatchMode=yes "$User@$ServerHost" "powershell -NoProfile -EncodedCommand $encoded" 2>&1
$rc = $LASTEXITCODE
if ($rc -ne 0) {
    Write-Error "[start-video] FAILED rc=$rc"
    exit $rc
}
Write-Host "[start-video] DONE" -ForegroundColor Green
