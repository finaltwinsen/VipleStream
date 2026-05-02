$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Continue'

Write-Output ''
Write-Output '=== Service status ==='
Get-Service VipleStreamServer | Format-Table Name, Status, StartType -AutoSize

Write-Output ''
Write-Output '=== Web UI 401 probe ==='
$resp = $null
$err  = $null
try {
    $resp = Invoke-WebRequest -UseBasicParsing -SkipCertificateCheck `
                              -Uri 'https://localhost:47990/' `
                              -TimeoutSec 10 -MaximumRedirection 0 `
                              -ErrorAction Stop
} catch {
    $err = $_
    $resp = $err.Exception.Response
}

if ($resp) {
    if ($resp.StatusCode) {
        $code = if ($resp.StatusCode.value__) { [int]$resp.StatusCode } else { $resp.StatusCode }
        Write-Output ('HTTP status: ' + $code)
    }
    $auth = $null
    try {
        if ($resp.Headers.GetValues) {
            $auth = ($resp.Headers.GetValues('WWW-Authenticate')) -join ', '
        } elseif ($resp.Headers['WWW-Authenticate']) {
            $auth = $resp.Headers['WWW-Authenticate']
        }
    } catch {}
    if ($auth) {
        Write-Output ('WWW-Authenticate: ' + $auth)
        if ($auth -match 'VipleStream-Server Web UI') {
            Write-Output 'OK realm rebrand verified'
        } elseif ($auth -match 'Sunshine Gamestream Host') {
            Write-Output 'WARNING old realm still served — exe likely not refreshed'
        } else {
            Write-Output 'WARNING realm string unexpected'
        }
    } else {
        Write-Output 'No WWW-Authenticate header in response'
    }
} else {
    Write-Output ('No response. Exception: ' + $err.Exception.Message)
}

Write-Output ''
Write-Output '=== Deployed exe ==='
Get-Item 'C:\Program Files\VipleStream-Server\viplestream-server.exe' |
    Format-Table Name, LastWriteTime, Length -AutoSize
