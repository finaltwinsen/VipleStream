# Restore firewall to pre-lockdown state
# Run on <host> as Administrator

$ErrorActionPreference = 'Stop'

Write-Host "=== Restore DefaultInboundAction = NotConfigured (effective default) ==="
Set-NetFirewallProfile -Profile Domain,Private,Public -DefaultInboundAction NotConfigured

Write-Host "=== Re-enable previously disabled Sunshine rules ==="
$restored = 0
$sunRules = Get-NetFirewallRule -Direction Inbound -Enabled False |
    Where-Object { $_.DisplayName -like '*Sunshine*' -or $_.DisplayName -like '*sunshine*' }
foreach ($r in $sunRules) {
    if ($r.Action -eq 'Allow') {
        Enable-NetFirewallRule -Name $r.Name
        $restored++
    }
}
Write-Host ("  Re-enabled {0} Sunshine rules" -f $restored)

Write-Host "=== Remove VipleStream-added rules ==="
$viple = Get-NetFirewallRule | Where-Object { $_.DisplayName -like 'VipleStream:*' }
foreach ($r in $viple) {
    Remove-NetFirewallRule -Name $r.Name
    Write-Host ("  Removed: {0}" -f $r.DisplayName)
}

Write-Host ""
Write-Host "=== Current state ==="
Get-NetFirewallProfile | Select-Object Name, Enabled, DefaultInboundAction, DefaultOutboundAction |
    Format-Table -AutoSize
Write-Host "=== DONE ==="
