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
# Critical: Plain Start-Process from SSH session (services session 0) does
# NOT cross into user's interactive console session — GUI app immediately
# exits without rendering.  schtasks /it also doesn't reliably cross
# session boundary.  Use Sysinternals PsExec -i 1 to launch into the
# logged-on user's interactive session 1.
# Pre-req: C:\Temp\PsExec64.exe must exist (deploy via scp once).
$serverScript = @"
`$ErrorActionPreference = 'Continue'

# Kill existing PotPlayer if any (does NOT need PsExec — Stop-Process from
# session 0 can kill GUI processes in session 1 if user has admin rights).
Get-Process PotPlayerMini64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600

# Verify paths
if (-not (Test-Path '$PotPlayer')) {
    Write-Error 'PotPlayer not found at $PotPlayer'
    exit 1
}
if (-not (Test-Path '$VideoPath')) {
    Write-Error 'Video file not found at $VideoPath'
    exit 1
}
if (-not (Test-Path 'C:\Temp\PsExec64.exe')) {
    Write-Error 'PsExec64.exe not found at C:\Temp\ — run scp PsExec64.exe to server first'
    exit 1
}

# PsExec -i 1 -d <exe> args : interactive in session 1, detached return.
# /fullscreen launches fullscreen.  Output gets routed via PowerShell
# stderr-as-output coloring but that's fine, it's informational.
& C:\Temp\PsExec64.exe -accepteula -nobanner -i 1 -d '$PotPlayer' '$VideoPath' /fullscreen 2>&1 | Out-Null

# Give PotPlayer a moment to render first frame in session 1
Start-Sleep -Seconds 2

# Verify it landed in session 1 (where the user can see it)
`$p = Get-Process PotPlayerMini64 -ErrorAction SilentlyContinue
if (`$p -and `$p.SessionId -eq 1) {
    Write-Output ('OK PotPlayer running PID=' + `$p.Id + ' Session=' + `$p.SessionId)
    exit 0
} elseif (`$p) {
    Write-Error ('PotPlayer running but in wrong session ' + `$p.SessionId + ' (expected 1)')
    exit 1
} else {
    Write-Error 'PotPlayer immediately exited (PsExec launch failed)'
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
