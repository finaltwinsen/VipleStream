@echo off

rem Get viplestream-server root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set RULE_NAME=VipleStream-Server
set PROGRAM_BIN="%ROOT_DIR%\viplestream-server.exe"

rem Add the rule
netsh advfirewall firewall add rule name=%RULE_NAME% dir=in action=allow protocol=tcp program=%PROGRAM_BIN% enable=yes
netsh advfirewall firewall add rule name=%RULE_NAME% dir=in action=allow protocol=udp program=%PROGRAM_BIN% enable=yes

rem Q-DSCP: tag MP-QUIC traffic (UDP source port 48010) as DSCP 40 (CS5), the same
rem class the UDP video socket gets from qWAVE. Wi-Fi APs map CS5 to the WMM video
rem queue; untagged QUIC lands in best effort and loses bursts of packets on Wi-Fi.
rem qWAVE cannot tag the shared QUIC socket (QOSAddSocketToFlow fails), so use a
rem persistent policy-based QoS rule instead. Port 48010 = default port 47989 + 21.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Remove-NetQosPolicy -Name 'VipleStream-Server-QUIC' -Confirm:$false -ErrorAction SilentlyContinue; New-NetQosPolicy -Name 'VipleStream-Server-QUIC' -AppPathNameMatchCondition 'viplestream-server.exe' -IPProtocolMatchCondition UDP -IPSrcPortStartMatchCondition 48010 -IPSrcPortEndMatchCondition 48010 -DSCPAction 40 -NetworkProfile All | Out-Null"
