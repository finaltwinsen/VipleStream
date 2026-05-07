#requires -Version 5.1
# §B-DUMP-BUILD 2026-05-07
# Compiles all d3d11 .hlsl shaders to .fxc.  Called from
# scripts/build_moonlight_package.cmd before staging step.  Wrote in
# PowerShell because cmd batch can't reliably escape /D macro args
# with embedded quotes (the previous attempt silently dropped 2nd /D).
#
# Always recompiles (fxc is fast enough that staleness check isn't
# worth the complexity).  Exit 0 even on individual shader compile
# failure — staging step will warn about missing .fxc and ship anyway.

$ErrorActionPreference = "Continue"

# Resolve fxc.exe — try PATH first, then known SDK location.
$fxc = (Get-Command fxc.exe -ErrorAction SilentlyContinue).Source
if (-not $fxc) {
    $fxc = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
    if (-not (Test-Path $fxc)) {
        # Try other Win10 SDK versions
        $candidates = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path "$($_.FullName)\x64\fxc.exe" } |
            Sort-Object Name -Descending
        if ($candidates) {
            $fxc = "$($candidates[0].FullName)\x64\fxc.exe"
        }
    }
}
if (-not (Test-Path $fxc)) {
    Write-Host "[WARN] fxc.exe not found - skipping shader compile (will ship existing .fxc)" -ForegroundColor Yellow
    exit 0
}

$shaderDir = Join-Path $PSScriptRoot "..\moonlight-qt\app\shaders"
if (-not (Test-Path $shaderDir)) {
    Write-Host "[WARN] shader dir not found: $shaderDir" -ForegroundColor Yellow
    exit 0
}

Set-Location $shaderDir

function Compile-Shader {
    param(
        [string]$OutBase,    # e.g. "d3d11_warp_quality" → output d3d11_warp_quality.fxc
        [string]$SrcHlsl,    # e.g. "d3d11_warp_compute.hlsl"
        [string]$Profile,    # cs_5_0 / vs_5_0 / ps_5_0
        [string[]]$Defines   # array of "NAME=VALUE" pairs
    )
    if (-not (Test-Path $SrcHlsl)) {
        Write-Host "  [SKIP] $SrcHlsl missing" -ForegroundColor DarkYellow
        return
    }
    $args = @("/T", $Profile, "/O3", "/nologo")
    foreach ($d in $Defines) {
        $args += @("/D", $d)
    }
    $args += @("/Fo", "$OutBase.fxc", $SrcHlsl)
    & $fxc @args 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  fxc $OutBase.fxc <- $SrcHlsl" -ForegroundColor DarkGreen
    } else {
        Write-Host "  [FAIL] fxc $OutBase failed (exit $LASTEXITCODE)" -ForegroundColor Red
    }
}

Write-Host "[shader-compile] using $fxc" -ForegroundColor Cyan

# Vertex / pixel shaders (no macros)
Compile-Shader "d3d11_vertex"               "d3d11_vertex.hlsl"               "vs_5_0" @()
Compile-Shader "d3d11_overlay_pixel"        "d3d11_overlay_pixel.hlsl"        "ps_5_0" @()
Compile-Shader "d3d11_yuv420_pixel"         "d3d11_yuv420_pixel.hlsl"         "ps_5_0" @()
Compile-Shader "d3d11_ayuv_pixel"           "d3d11_ayuv_pixel.hlsl"           "ps_5_0" @()
Compile-Shader "d3d11_y410_pixel"           "d3d11_y410_pixel.hlsl"           "ps_5_0" @()
if (Test-Path "d3d11_bicubic_scale_pixel.hlsl") {
    Compile-Shader "d3d11_bicubic_scale_pixel" "d3d11_bicubic_scale_pixel.hlsl" "ps_5_0" @()
}

# Compute shaders (single-variant)
Compile-Shader "d3d11_motionest_compute"    "d3d11_motionest_compute.hlsl"    "cs_5_0" @()
Compile-Shader "d3d11_warp_compute"         "d3d11_warp_compute.hlsl"         "cs_5_0" @()
Compile-Shader "d3d11_mv_median"            "d3d11_mv_median.hlsl"            "cs_5_0" @()
Compile-Shader "d3d11_dml_pack_rgba8_fp16"  "d3d11_dml_pack_rgba8_fp16.hlsl"  "cs_5_0" @()
Compile-Shader "d3d11_dml_unpack_fp16_rgba8" "d3d11_dml_unpack_fp16_rgba8.hlsl" "cs_5_0" @()
Compile-Shader "d3d11_fruc_blend_fp32"      "d3d11_fruc_blend_fp32.hlsl"      "cs_5_0" @()

# 3 quality variants of motionest (QUALITY_LEVEL macro)
Compile-Shader "d3d11_motionest_quality"     "d3d11_motionest_compute.hlsl" "cs_5_0" @("QUALITY_LEVEL=0")
Compile-Shader "d3d11_motionest_balanced"    "d3d11_motionest_compute.hlsl" "cs_5_0" @("QUALITY_LEVEL=1")
Compile-Shader "d3d11_motionest_performance" "d3d11_motionest_compute.hlsl" "cs_5_0" @("QUALITY_LEVEL=2")

# 3 quality variants of warp (different blend mode flags)
Compile-Shader "d3d11_warp_quality"     "d3d11_warp_compute.hlsl" "cs_5_0" @("ENABLE_ADAPTIVE_BLEND=1", "ENABLE_CHEAP_ADAPTIVE=0")
Compile-Shader "d3d11_warp_balanced"    "d3d11_warp_compute.hlsl" "cs_5_0" @("ENABLE_ADAPTIVE_BLEND=0", "ENABLE_CHEAP_ADAPTIVE=1")
Compile-Shader "d3d11_warp_performance" "d3d11_warp_compute.hlsl" "cs_5_0" @("ENABLE_ADAPTIVE_BLEND=0", "ENABLE_CHEAP_ADAPTIVE=0")

Write-Host "[shader-compile] done" -ForegroundColor Cyan
exit 0
