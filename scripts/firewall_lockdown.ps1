# VipleStream firewall lockdown (scenario ii: force hole-punch)
# Run on <host> as Administrator
#
# Effect:
#   - Inbound default: Block (all profiles)
#   - Allow: SSH (TCP 22), WOL (UDP 7/9), ICMPv4 Echo (for debugging)
#   - Block: All Sunshine inbound (TCP 47984/47989/47990/48010, UDP 47998-48000/48002)
#   - Outbound: unchanged (Allow default)
# Sunshine must use:
#   - RTSP via relay WebSocket tunnel → 127.0.0.1 (loopback, not firewalled)
#   - Video/audio/control via UDP hole-punch (stateful allowed as replies to outbound)
#
# SAFETY:
#   - SSH allow rule is CREATED BEFORE DefaultInboundAction=Block is applied.
#   - Existing Sunshine rules are backed up to C:\Temp\firewall_backup.json first.

$ErrorActionPreference = 'Stop'

$BackupPath = 'C:\Temp\firewall_backup.json'
New-Item -ItemType Directory -Path 'C:\Temp' -Force | Out-Null

Write-Host "=== Step 1: Backup current Sunshine firewall rules ==="
$sunRules = Get-NetFirewallRule -Direction Inbound |
    Where-Object { $_.DisplayName -like '*Sunshine*' -or $_.DisplayName -like '*sunshine*' }
$backup = @()
foreach ($r in $sunRules) {
    $backup += [PSCustomObject]@{
        Name          = $r.Name
        DisplayName   = $r.DisplayName
        Enabled       = $r.Enabled.ToString()
        Action        = $r.Action.ToString()
        Direction     = $r.Direction.ToString()
        Profile       = $r.Profile.ToString()
    }
}
$backup | ConvertTo-Json -Depth 4 | Set-Content -Path $BackupPath -Encoding UTF8
Write-Host ("  Backed up {0} Sunshine rules to {1}" -f $sunRules.Count, $BackupPath)

Write-Host ""
Write-Host "=== Step 2: Ensure SSH + WOL allow rules exist BEFORE default-block ==="

# SSH inbound (TCP 22)
$existingSSH = Get-NetFirewallRule -DisplayName 'VipleStream: SSH Inbound' -ErrorAction SilentlyContinue
if ($existingSSH) { Remove-NetFirewallRule -DisplayName 'VipleStream: SSH Inbound' }
New-NetFirewallRule -DisplayName 'VipleStream: SSH Inbound' `
    -Direction Inbound -Protocol TCP -LocalPort 22 `
    -Action Allow -Profile Any -Enabled True | Out-Null
Write-Host "  [OK] SSH Inbound (TCP 22) allowed"

# WOL UDP 7
$existingWOL7 = Get-NetFirewallRule -DisplayName 'VipleStream: WOL UDP 7' -ErrorAction SilentlyContinue
if ($existingWOL7) { Remove-NetFirewallRule -DisplayName 'VipleStream: WOL UDP 7' }
New-NetFirewallRule -DisplayName 'VipleStream: WOL UDP 7' `
    -Direction Inbound -Protocol UDP -LocalPort 7 `
    -Action Allow -Profile Any -Enabled True | Out-Null
Write-Host "  [OK] WOL UDP 7 allowed"

# WOL UDP 9
$existingWOL9 = Get-NetFirewallRule -DisplayName 'VipleStream: WOL UDP 9' -ErrorAction SilentlyContinue
if ($existingWOL9) { Remove-NetFirewallRule -DisplayName 'VipleStream: WOL UDP 9' }
New-NetFirewallRule -DisplayName 'VipleStream: WOL UDP 9' `
    -Direction Inbound -Protocol UDP -LocalPort 9 `
    -Action Allow -Profile Any -Enabled True | Out-Null
Write-Host "  [OK] WOL UDP 9 allowed"

# ICMPv4 Echo (ping) — useful for connectivity diagnosis
$existingICMP = Get-NetFirewallRule -DisplayName 'VipleStream: ICMPv4 Echo' -ErrorAction SilentlyContinue
if ($existingICMP) { Remove-NetFirewallRule -DisplayName 'VipleStream: ICMPv4 Echo' }
New-NetFirewallRule -DisplayName 'VipleStream: ICMPv4 Echo' `
    -Direction Inbound -Protocol ICMPv4 -IcmpType 8 `
    -Action Allow -Profile Any -Enabled True | Out-Null
Write-Host "  [OK] ICMPv4 Echo Request allowed"

Write-Host ""
Write-Host "=== Step 3: Disable all Sunshine inbound allow rules ==="
$disabled = 0
foreach ($r in $sunRules) {
    if ($r.Enabled -eq 'True' -and $r.Action -eq 'Allow' -and $r.Direction -eq 'Inbound') {
        Disable-NetFirewallRule -Name $r.Name
        $disabled++
    }
}
Write-Host ("  Disabled {0} Sunshine inbound allow rules" -f $disabled)

# Also disable any generic port-based allow rules for Sunshine ports
$portRules = Get-NetFirewallRule -Direction Inbound -Enabled True -Action Allow |
    Get-NetFirewallPortFilter |
    Where-Object {
        ($_.Protocol -eq 'TCP' -and ($_.LocalPort -in @('47984','47989','47990','48010'))) -or
        ($_.Protocol -eq 'UDP' -and ($_.LocalPort -in @('47998','47999','48000','48002')))
    }
$portRuleCount = 0
foreach ($pf in $portRules) {
    $rule = Get-NetFirewallRule -Name $pf.InstanceID -ErrorAction SilentlyContinue
    if ($rule -and $rule.Enabled -eq 'True') {
        Disable-NetFirewallRule -Name $rule.Name
        Write-Host ("    also disabled: {0}" -f $rule.DisplayName)
        $portRuleCount++
    }
}
Write-Host ("  Disabled {0} additional port-based allow rules" -f $portRuleCount)

Write-Host ""
Write-Host "=== Step 4: Set DefaultInboundAction=Block on all profiles ==="
Set-NetFirewallProfile -Profile Domain,Private,Public `
    -DefaultInboundAction Block `
    -DefaultOutboundAction Allow `
    -Enabled True
Get-NetFirewallProfile | Select-Object Name, Enabled, DefaultInboundAction, DefaultOutboundAction |
    Format-Table -AutoSize

Write-Host ""
Write-Host "=== Step 5: Verify Sunshine service still running ==="
$svc = Get-Service SunshineService
Write-Host ("  SunshineService: {0}" -f $svc.Status)

Write-Host ""
Write-Host "=== DONE ==="
Write-Host "Revert with: firewall_restore.ps1"
