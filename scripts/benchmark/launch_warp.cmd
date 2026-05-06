@echo off
:: launch_warp.cmd — VipleStream client launcher 切換 FRUC warp blend 模式
::
:: 用法：
::   scripts\benchmark\launch_warp.cmd            (預設 cheap-adaptive)
::   scripts\benchmark\launch_warp.cmd no_mv      (c2: 跳過 ME，純 same-pixel cross-fade)
::   scripts\benchmark\launch_warp.cmd quality    (c1: d3d11 Quality 雙向一致性)
::   scripts\benchmark\launch_warp.cmd pure50     (c0: 固定 50/50 blend)
::   scripts\benchmark\launch_warp.cmd crgb_real  (d: real 也走 compute-NV12→RGB；治本修法)
::   scripts\benchmark\launch_warp.cmd triple     (B2: 60→180 補幀×3，需 180Hz display)
::   scripts\benchmark\launch_warp.cmd nvof       (B-NVOF: 啟用 VK_NV_optical_flow，目前只到 scaffolding)
::
:: 寫成 .cmd 是因為 PowerShell 的 `set X=Y && app.exe` 不會設環境變數
:: (PS5 不認 &&；set 是 Set-Variable alias 不是 export).

setlocal
cd /d "%~dp0\..\.."

set "MODE=%~1"
if /I "%MODE%"=="no_mv"     set "VIPLE_VKFRUC_WARP_NO_MV=1"     & set "MODE_LABEL=c2 no-MV (DIAG)"
if /I "%MODE%"=="quality"   set "VIPLE_VKFRUC_WARP_QUALITY=1"   & set "MODE_LABEL=c1 Quality adaptive"
if /I "%MODE%"=="pure50"    set "VIPLE_VKFRUC_WARP_PURE50=1"    & set "MODE_LABEL=c0 fixed 50/50"
if /I "%MODE%"=="crgb_real" set "VIPLE_VKFRUC_REAL_USE_CRGB=1"  & set "MODE_LABEL=d Real path through compute-NV12-RGB"
if /I "%MODE%"=="triple"    set "VIPLE_VKFRUC_TRIPLE=1"         & set "MODE_LABEL=B2 TRIPLE 60-180 (3x interp)" & set "FPS=90"
if /I "%MODE%"=="nvof"      set "VIPLE_VKFRUC_NV_OF=1"          & set "MODE_LABEL=B-NVOF VK_NV_optical_flow scaffolding"
if not defined MODE_LABEL    set "MODE_LABEL=Balanced cheap-adaptive (default)"
if not defined FPS           set "FPS=60"

echo [launch_warp] mode = %MODE_LABEL%
echo [launch_warp] fps  = %FPS%  (client display target; server fps = FPS / 2 in DUAL or / 3 in TRIPLE)
echo [launch_warp] env  = NO_MV=%VIPLE_VKFRUC_WARP_NO_MV% QUALITY=%VIPLE_VKFRUC_WARP_QUALITY% PURE50=%VIPLE_VKFRUC_WARP_PURE50% REAL_USE_CRGB=%VIPLE_VKFRUC_REAL_USE_CRGB% TRIPLE=%VIPLE_VKFRUC_TRIPLE% NV_OF=%VIPLE_VKFRUC_NV_OF%

temp\moonlight\VipleStream.exe stream 192.168.51.226 Desktop ^
    --1080 --fps %FPS% --video-codec H.264 --frame-interpolation --no-yuv444
