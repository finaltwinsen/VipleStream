Get-Service SunshineService | Format-List Status
$l = 'C:\Program Files\Sunshine\config\sunshine.log'
if (Test-Path $l) {
    '--- last 30 lines ---'
    Get-Content $l -Tail 30
}
