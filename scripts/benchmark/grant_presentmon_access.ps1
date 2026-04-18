# =============================================================================
#  One-time setup: allow current user to run PresentMon without UAC
#
#  PresentMon uses Windows ETW which normally requires administrator privileges.
#  Adding the user to the built-in "Performance Log Users" group grants the
#  SeSystemProfilePrivilege needed for ETW sessions, so subsequent runs work
#  without elevation.
#
#  Run once with admin privileges, then log off and log back in for the new
#  group membership to take effect in your user token.
# =============================================================================

$ErrorActionPreference = 'Stop'

# Use SIDs instead of names to avoid locale issues on non-English Windows.
# S-1-5-32-559 = Performance Log Users
# S-1-5-32-573 = Event Log Readers
$PerfLogSid = 'S-1-5-32-559'
$EvtLogSid  = 'S-1-5-32-573'

function Get-LocalGroupName([string]$Sid) {
    try {
        $g = Get-LocalGroup -SID $Sid -ErrorAction Stop
        return $g.Name
    } catch { return $null }
}

function Test-Admin {
    ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---- Self-elevate ---------------------------------------------------------
if (-not (Test-Admin)) {
    Write-Host "Requesting admin elevation..." -ForegroundColor Yellow
    $scriptPath = $MyInvocation.MyCommand.Path
    Start-Process -FilePath 'powershell.exe' `
        -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$scriptPath) `
        -Verb RunAs
    exit 0
}

# ---- Proceed as admin -----------------------------------------------------
$user = $env:USERNAME
Write-Host ""
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "  VipleStream - Grant PresentMon (ETW) access to $user" -ForegroundColor Cyan
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host ""

$perfGroup = Get-LocalGroupName $PerfLogSid
$evtGroup  = Get-LocalGroupName $EvtLogSid
if (-not $perfGroup) { throw "'Performance Log Users' group (SID $PerfLogSid) not found on this system" }

Write-Host "[1/3] Adding $user to '$perfGroup' ($PerfLogSid)..."
try {
    Add-LocalGroupMember -SID $PerfLogSid -Member $user -ErrorAction Stop
    Write-Host "      Added." -ForegroundColor Green
} catch {
    if ($_.Exception.Message -match 'already a member' -or $_ -match 'already a member') {
        Write-Host "      Already a member." -ForegroundColor DarkGray
    } else {
        Write-Warning "Add-LocalGroupMember failed: $($_.Exception.Message)"
        Write-Host "      Trying 'net localgroup' fallback..."
        & net localgroup $perfGroup $user /add 2>&1 | Out-Host
    }
}

Write-Host ""
Write-Host "[2/3] Adding $user to '$evtGroup' ($EvtLogSid) (belt-and-braces)..."
if ($evtGroup) {
    try {
        Add-LocalGroupMember -SID $EvtLogSid -Member $user -ErrorAction Stop
        Write-Host "      Added." -ForegroundColor Green
    } catch {
        if ($_.Exception.Message -match 'already a member' -or $_ -match 'already a member') {
            Write-Host "      Already a member." -ForegroundColor DarkGray
        } else {
            Write-Warning "  Skipped: $($_.Exception.Message)"
        }
    }
} else {
    Write-Host "      (group not present, skipped)" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "[3/3] Verifying membership (from new token via 'net localgroup')..."
$members = & net localgroup $perfGroup 2>$null
$present = $members | Where-Object { $_ -match [regex]::Escape($user) }
if ($present) {
    Write-Host "      $user is a member of '$perfGroup'." -ForegroundColor Green
} else {
    Write-Warning "      $user NOT yet visible as a member — re-check with 'net localgroup ""$perfGroup""'"
}

$tokenHas = (whoami /groups) -match [regex]::Escape($perfGroup)
if ($tokenHas) {
    Write-Host "      Current token already contains the group — ready to use." -ForegroundColor Green
} else {
    Write-Host "      Current token does NOT yet contain the group." -ForegroundColor Yellow
    Write-Host "      You MUST log off and log back in (or reboot) for the group" -ForegroundColor Yellow
    Write-Host "      to be included in new session tokens." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "  Setup complete."
Write-Host ""
Write-Host "  Next step:"
Write-Host "    1. Log off Windows (Start > user > Sign out)"
Write-Host "    2. Log back in"
Write-Host "    3. Re-run scripts\benchmark\run_fruc_30s.cmd (no UAC)"
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host ""
Read-Host 'Press Enter to close'
