@echo off

set RULE_NAME=VipleStream-Server

rem Delete the rule (and the legacy "Sunshine" rule if present)
netsh advfirewall firewall delete rule name=%RULE_NAME%
netsh advfirewall firewall delete rule name=Sunshine 2>nul
