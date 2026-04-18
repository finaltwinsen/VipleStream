@echo off
:: One-time: grant current user ETW access so PresentMon runs without UAC.
:: Launches grant_presentmon_access.ps1 which self-elevates via PowerShell.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0grant_presentmon_access.ps1"
