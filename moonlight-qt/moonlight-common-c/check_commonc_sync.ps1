#requires -Version 5
<#
  §F1（VipleStream 2.0 M0 前置修正）：三份 moonlight-common-c 同步檢查

  專案裡有三份 moonlight-common-c：
    Q  = moonlight-qt/moonlight-common-c/moonlight-common-c/              （正本）
    A  = moonlight-android/app/src/main/jni/moonlight-core/moonlight-common-c/
    S3 = Sunshine/third-party/moonlight-common-c/

  為什麼要有這支檢查：A 曾經長期落後 Q（src/ 7 檔、enet/ 5 檔），其中
  RtpVideoQueue.c 的 isBefore16 迴繞 bug（§FRZ-B1）已在 Q 修掉，Android 卻
  一直帶著會永久凍結畫面的舊版。2.0 的 VR 協定又依賴 ENet 的 unsequenced 與
  throttle 行為，兩端 ENet 若悄悄分岔，VR 會以很難追的方式壞掉。所以同步
  必須由 build 把關，不能靠人記得。

  檢查規則（詳見 docs/vr_protocol.md §4.8）：
    (1) Q 對 A：src/ 與 enet/ 底下全部檔案，行尾正規化（CRLF→LF）後必須
        byte-identical，兩邊的檔案集合也必須相同。白名單只有下方
        $QaWhitelist 列出的檔案，每一項都附理由。
    (2) Q 對 S3：server 實際編譯、且目前一致的檔案必須維持一致（清單與依據
        見 $QsRequired）。
    (3) 其他漂移（Q 對 S3 的其餘檔案、Q 對 A 的 nanors/）只印警告，不擋 build。

  檔案集合怎麼決定：
    - RepoRoot 是 git 工作樹根目錄時，用「git 追蹤檔（扣掉工作樹已刪除的）＋
      未追蹤且未被 .gitignore 排除的檔」，內容一律讀工作樹。未 stage 的刪除、
      還沒 git add 的新檔都會印 [WARN]：本檢查看的是工作樹，commit 時漏掉
      這些變更，commit 內容就會和檢查結果不符。
    - 未追蹤、又不是原始碼副檔名（見 $SourceExt）的檔案視為雜檔（*.orig、
      編輯器備份等），只警告、不列入比對，避免誤判 FAIL。
    - 不是 git 工作樹（例如負向測試用的複製樹）或找不到 git 時，退回列出
      工作樹全部檔案、全部嚴格比對。

  用法：
    powershell -NoProfile -ExecutionPolicy Bypass -File check_commonc_sync.ps1
    （可加 -RepoRoot <repo 根目錄>；預設為本腳本往上兩層）
    Windows PowerShell 5.1 與 pwsh 7 都可以跑；路徑一律用正斜線組合，
    日後 linux-builder 也能直接用。

  Exit code：
    0  PASS（可能帶警告）
    1  違反 (1) 或 (2)，或找不到必要目錄、git 查詢失敗 → 呼叫端必須讓 build 失敗

  輸出：每行以 [VIPLE-COMMONC-SYNC] 開頭；違規行含 [ERROR]、警告行含 [WARN]，
  build log 只需 grep [ERROR] 即可判斷。規則編號刻意用 ASCII 的 (1)(2)(3)：
  PowerShell 5.1 導向檔案時用系統 ANSI 字碼頁（cp950）輸出，圈號數字會變成 '?'。
#>
param(
    [string]$RepoRoot = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

if ([string]::IsNullOrEmpty($RepoRoot)) {
    $RepoRoot = Join-Path $PSScriptRoot '../..'
}
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

$Tag = '[VIPLE-COMMONC-SYNC]'
# repo 相對路徑（正斜線）：同時用來組檔案路徑與當 git pathspec。
$RelQ  = 'moonlight-qt/moonlight-common-c/moonlight-common-c'
$RelA  = 'moonlight-android/app/src/main/jni/moonlight-core/moonlight-common-c'
$RelS3 = 'Sunshine/third-party/moonlight-common-c'
$DirQ  = Join-Path $RepoRoot $RelQ
$DirA  = Join-Path $RepoRoot $RelA
$DirS3 = Join-Path $RepoRoot $RelS3

# ── (1) Q 對 A 的白名單 ───────────────────────────────────────────
# key = 相對路徑（正斜線），value = 理由。白名單檔案不做 byte 比對，但
# 會改做「結構化比對」（見 Test-PlatformNetIf），不是完全放生。
#
# §F1 評估結論（2026-09-23）：enet/ 的 host.c、protocol.c、enet.h、unix.c、
# win32.c 五檔的差異全部是 A 落後上游 ENet（wildcardBind、wait timeout 計算、
# Wii U／Vita／Haiku／FreeBSD 支援、1U 位移修正），沒有任何 Android 專屬修改，
# 因此全部以 Q 覆寫，不列入白名單。
$QaWhitelist = @{
    'src/PlatformNetIf.h' = 'Android 版多一段 #ifdef __ANDROID__ 的 lcSetNetInterfacesFromJni 宣告（JNI 層 callbacks.c 注入介面清單用；實作已在兩邊共用的 PlatformNetIf.c）。檢查方式：A 去掉該區塊後必須與 Q 完全一致，且該區塊必須存在。'
}

# ── (2) Q 對 S3 必須一致的檔案 ────────────────────────────────────
# 只列「server 實際編譯」且「目前一致」的檔案：
#   - Sunshine/cmake/compile_definitions/common.cmake 的 SUNSHINE_TARGET_FILES：
#     Input.h、Rtsp.h、RtspParser.c、Video.h（wire 格式）。
#   - server 經 include 鏈實際編譯的 header：Sunshine/src/stream.cpp 與
#     rtsp.cpp 都 #include <moonlight-common-c/src/Limelight-internal.h>，它再
#     include PlatformThreads.h、Video.h、Input.h、RtpAudioQueue.h、ByteBuffer.h；
#     Video.h → LinkedBlockingQueue.h；RtpAudioQueue.h → "rswrapper.h"（引號
#     include 先找同目錄，所以用的是 S3/src/rswrapper.h）。其中 RtpAudioQueue.h
#     定義音訊 FEC 的 wire 結構 AUDIO_FEC_HEADER 與 RTPA_DATA_SHARDS／
#     RTPA_FEC_SHARDS，stream.cpp 直接拿來組封包與建 reed_solomon，Q 端改了
#     而 server 沒跟上，音訊 FEC 就會不相容。
#     同一條 include 鏈上的 Limelight-internal.h、Limelight.h、Platform.h、
#     PlatformSockets.h、PlatformCrypto.h、RtpVideoQueue.h 目前 Q/S3 已經不同，
#     依規則不列入，歸 (3) 警告。
#   - Sunshine/cmake/dependencies/common.cmake 的
#     add_subdirectory(third-party/moonlight-common-c/enet)：ENet 本體。VR 依賴
#     兩端 ENet 的 unsequenced／throttle 行為一致（docs/vr_architecture.md F1）。
$QsRequired = @(
    # VipleVr.h：2.0 VR 協定的單一定義來源（ptype、0x81 header、tracking、TLV），
    # stream.cpp 直接 include，三份必須 byte-identical（docs/vr_protocol.md §4.8）。
    'src/VipleVr.h',
    'src/Input.h',
    'src/Video.h',
    'src/Rtsp.h',
    'src/RtspParser.c',
    'src/RtpAudioQueue.h',
    'src/PlatformThreads.h',
    'src/ByteBuffer.h',
    'src/LinkedBlockingQueue.h',
    'src/rswrapper.h',
    'enet/callbacks.c',
    'enet/compress.c',
    'enet/host.c',
    'enet/list.c',
    'enet/packet.c',
    'enet/peer.c',
    'enet/protocol.c',
    'enet/unix.c',
    'enet/win32.c',
    'enet/include/enet/callbacks.h',
    'enet/include/enet/enet.h',
    'enet/include/enet/list.h',
    'enet/include/enet/protocol.h',
    'enet/include/enet/time.h',
    'enet/include/enet/types.h',
    'enet/include/enet/unix.h',
    'enet/include/enet/utility.h',
    'enet/include/enet/win32.h'
)

# (2) 裡 #include 行的大小寫不列入比對的檔案：S3 把 <Mswsock.h>、
# <VersionHelpers.h>、<Ws2tcpip.h> 改成小寫，供 MinGW 在大小寫敏感的檔案系統
# 交叉編譯；Q 由 MSVC 編譯維持上游大小寫。Windows 檔案系統不分大小寫，兩者
# 語意相同。只把 #include 的目標轉小寫再比對，其餘內容仍須完全一致。
$QsIncludeCaseInsensitive = @(
    'enet/win32.c',
    'enet/include/enet/win32.h'
)

# 未追蹤檔案中「視為原始碼」的副檔名；其他未追蹤檔案當雜檔處理。
$SourceExt = @('.c', '.h', '.cc', '.cpp', '.hpp', '.inc', '.def')

$script:Errors = 0
$script:Warnings = 0

function Write-Err([string]$msg) {
    $script:Errors++
    Write-Host "$Tag [ERROR] $msg"
}

function Write-Warn([string]$msg) {
    $script:Warnings++
    Write-Host "$Tag [WARN] $msg"
}

# Latin-1 是 1 byte 對 1 字元的無損映射：以它解碼再把 CRLF 換成 LF，
# 比對結果等同「行尾正規化後的 byte 比對」，不受檔案實際編碼影響。
$Latin1 = [System.Text.Encoding]::GetEncoding(28591)

function Get-NormalizedText([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes((Convert-Path -LiteralPath $path))
    return $Latin1.GetString($bytes).Replace("`r`n", "`n")
}

# 只把 #include <...> / "..." 的目標轉小寫（見 $QsIncludeCaseInsensitive）。
function Get-IncludeCaseFolded([string]$text) {
    return [regex]::Replace($text, '(?m)^([ \t]*#[ \t]*include[ \t]*)([<"][^>"\n]*[>"])', {
        param($m) $m.Groups[1].Value + $m.Groups[2].Value.ToLowerInvariant()
    })
}

# 回傳第一個不同的行號（1 起算），給違規訊息定位用。
function Get-FirstDiffLine([string]$a, [string]$b) {
    $la = $a.Split("`n")
    $lb = $b.Split("`n")
    $n = [Math]::Min($la.Length, $lb.Length)
    for ($i = 0; $i -lt $n; $i++) {
        if (-not [string]::Equals($la[$i], $lb[$i], [System.StringComparison]::Ordinal)) {
            return $i + 1
        }
    }
    return $n + 1
}

# ── git 追蹤資訊 ─────────────────────────────────────────────────
# PowerShell 5.1 在 $ErrorActionPreference='Stop' 下，native 指令寫 stderr 會被
# 當成終止錯誤；呼叫 git 時暫時改成 Continue，改以 exit code 判斷。
function Invoke-Git([string[]]$gitArgs) {
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & git -c core.quotepath=off -C $RepoRoot @gitArgs 2>$null
        return [pscustomobject]@{ Code = $LASTEXITCODE; Lines = @($out | Where-Object { $_ -ne '' }) }
    }
    finally {
        $ErrorActionPreference = $eap
    }
}

# RepoRoot 必須「就是」git 工作樹的根目錄才用 git 模式；複製樹若剛好位在別的
# repo 底下，不能誤用外層 repo 的索引。
function Test-GitMode {
    if ($null -eq (Get-Command git -ErrorAction SilentlyContinue)) { return $false }
    $r = Invoke-Git @('rev-parse', '--show-toplevel')
    if ($r.Code -ne 0 -or $r.Lines.Count -ne 1) { return $false }
    $top = [System.IO.Path]::GetFullPath($r.Lines[0]).TrimEnd('\', '/')
    $root = [System.IO.Path]::GetFullPath($RepoRoot).TrimEnd('\', '/')
    $cmp = [System.StringComparison]::Ordinal
    if ([System.IO.Path]::DirectorySeparatorChar -eq '\') { $cmp = [System.StringComparison]::OrdinalIgnoreCase }
    return [string]::Equals($top, $root, $cmp)
}

$UseGit = Test-GitMode

# 列出 base 底下指定子目錄的檔案。回傳物件：
#   Files     : 相對 base 的路徑（正斜線），代表工作樹目前實際存在、要比對的檔案
#   Untracked : Files 中尚未被 git 追蹤的檔案（非 git 模式恆為空）
#   Deleted   : git 追蹤、但工作樹已刪除且尚未 stage 的檔案（非 git 模式恆為空）
# 非 git 模式排除任何名為 .git 的路徑段：A/enet 底下有一個早年 submodule
# 殘留的未追蹤 .git 檔（gitdir 指標），不屬於原始碼。git 本身也不會列出它。
function Get-RelFiles([string]$relBase, [string[]]$subdirs) {
    $files = New-Object 'System.Collections.Generic.List[string]'
    $untracked = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
    $deleted = New-Object 'System.Collections.Generic.List[string]'
    $prefix = "$relBase/"

    if ($UseGit) {
        $specs = @($subdirs | ForEach-Object { "$relBase/$_" })
        $cached = Invoke-Git (@('ls-files', '--cached', '--') + $specs)
        $gone   = Invoke-Git (@('ls-files', '--deleted', '--') + $specs)
        $others = Invoke-Git (@('ls-files', '--others', '--exclude-standard', '--') + $specs)
        foreach ($r in @($cached, $gone, $others)) {
            if ($r.Code -ne 0) { throw "git ls-files 失敗（exit $($r.Code)）：$relBase" }
        }
        $goneSet = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
        foreach ($p in $gone.Lines) {
            if ($p.StartsWith($prefix)) {
                $rel = $p.Substring($prefix.Length)
                [void]$goneSet.Add($rel)
                $deleted.Add($rel)
            }
        }
        foreach ($p in $cached.Lines) {
            if (-not $p.StartsWith($prefix)) { continue }
            $rel = $p.Substring($prefix.Length)
            if ($goneSet.Contains($rel)) { continue }
            $files.Add($rel)
        }
        foreach ($p in $others.Lines) {
            if (-not $p.StartsWith($prefix)) { continue }
            $rel = $p.Substring($prefix.Length)
            $files.Add($rel)
            [void]$untracked.Add($rel)
        }
    }
    else {
        $baseFull = (Resolve-Path -LiteralPath (Join-Path $RepoRoot $relBase)).Path.TrimEnd('\', '/')
        foreach ($sub in $subdirs) {
            $dir = Join-Path $baseFull $sub
            if (-not (Test-Path -LiteralPath $dir)) { continue }
            foreach ($f in (Get-ChildItem -LiteralPath $dir -Recurse -File -Force)) {
                $rel = $f.FullName.Substring($baseFull.Length + 1).Replace('\', '/')
                if (($rel.Split('/') -contains '.git')) { continue }
                $files.Add($rel)
            }
        }
    }
    return [pscustomobject]@{ Files = $files; Untracked = $untracked; Deleted = $deleted }
}

function Test-IsSourceFile([string]$rel) {
    $ext = [System.IO.Path]::GetExtension($rel).ToLowerInvariant()
    return ($SourceExt -contains $ext)
}

# PlatformNetIf.h 的結構化比對：A 去掉 #ifdef __ANDROID__ … #endif 區塊
# （含其後一個空行）後必須與 Q 完全一致，且區塊內必須宣告
# lcSetNetInterfacesFromJni。回傳 $null 表示通過，否則回傳錯誤說明。
function Test-PlatformNetIf([string]$textQ, [string]$textA) {
    # 日後若把 Android 宣告直接合併進 Q（__ANDROID__ 守衛對桌面版無影響），
    # 兩邊完全相同也算通過。
    if ([string]::Equals($textQ, $textA, [System.StringComparison]::Ordinal) -and
        $textA -match 'lcSetNetInterfacesFromJni') {
        return $null
    }
    $pattern ='(?m)^#ifdef __ANDROID__\n(?:(?!#endif)[^\n]*\n)*?#endif\n\n?'
    $m = [regex]::Matches($textA, $pattern)
    if ($m.Count -ne 1) {
        return "A 應恰有一個 #ifdef __ANDROID__ 區塊，實際 $($m.Count) 個"
    }
    if ($m[0].Value -notmatch 'lcSetNetInterfacesFromJni') {
        return 'A 的 __ANDROID__ 區塊缺少 lcSetNetInterfacesFromJni 宣告（Android JNI 層需要）'
    }
    $stripped = $textA.Remove($m[0].Index, $m[0].Length)
    if (-not [string]::Equals($stripped, $textQ, [System.StringComparison]::Ordinal)) {
        $line = Get-FirstDiffLine $textQ $stripped
        return "A 去掉 __ANDROID__ 區塊後仍與 Q 不同（約第 $line 行起）"
    }
    return $null
}

# 比對兩棵樹同一個相對路徑；回傳 same / missing-both / missing-x / missing-y / diff@<行號>。
function Compare-Files([string]$baseX, [string]$baseY, [string]$rel, [switch]$FoldIncludeCase) {
    $px = Join-Path $baseX $rel
    $py = Join-Path $baseY $rel
    $ex = Test-Path -LiteralPath $px -PathType Leaf
    $ey = Test-Path -LiteralPath $py -PathType Leaf
    if (-not $ex -and -not $ey) { return 'missing-both' }
    if (-not $ex) { return 'missing-x' }
    if (-not $ey) { return 'missing-y' }
    $tx = Get-NormalizedText $px
    $ty = Get-NormalizedText $py
    if ($FoldIncludeCase) {
        $tx = Get-IncludeCaseFolded $tx
        $ty = Get-IncludeCaseFolded $ty
    }
    if ([string]::Equals($tx, $ty, [System.StringComparison]::Ordinal)) { return 'same' }
    return "diff@$(Get-FirstDiffLine $tx $ty)"
}

# ── 前置：三棵樹都要存在 ─────────────────────────────────────────
foreach ($pair in @(@('Q', $DirQ), @('A', $DirA), @('S3', $DirS3))) {
    if (-not (Test-Path -LiteralPath (Join-Path $pair[1] 'src'))) {
        Write-Err "找不到 $($pair[0]) 的 src/：$($pair[1])"
    }
}
if ($script:Errors -gt 0) {
    Write-Host "$Tag FAIL（目錄不完整）"
    exit 1
}

if ($UseGit) {
    Write-Host "$Tag 檔案集合：git 追蹤檔＋未追蹤檔（排除 .gitignore），內容讀工作樹"
}
else {
    Write-Host "$Tag 檔案集合：工作樹全部檔案（RepoRoot 不是 git 工作樹根目錄或找不到 git）"
}

# ── (1) Q 對 A：src/ + enet/ ────────────────────────────────────
$listQ = Get-RelFiles $RelQ @('src', 'enet')
$listA = Get-RelFiles $RelA @('src', 'enet')

# commit 一致性提示：檢查看的是工作樹，這些變更沒進 commit 就會和結果不符。
foreach ($side in @(@('Q', $listQ), @('A', $listA))) {
    foreach ($rel in $side[1].Deleted) {
        Write-Warn "(1) $($side[0]) 的 $rel 已從工作樹刪除但尚未 stage：commit 時要用 git rm（或 git add -A 該路徑）一併納入"
    }
    foreach ($rel in $side[1].Untracked) {
        if (Test-IsSourceFile $rel) {
            Write-Warn "(1) $($side[0]) 的 $rel 尚未 git add：commit 時要一併加入，否則 commit 內容與本檢查不符"
        }
    }
}

$union = New-Object 'System.Collections.Generic.SortedSet[string]' ([System.StringComparer]::Ordinal)
foreach ($f in $listQ.Files) { [void]$union.Add($f) }
foreach ($f in $listA.Files) { [void]$union.Add($f) }

$qaChecked = 0
foreach ($rel in $union) {
    # 未追蹤的非原始碼檔（*.orig、編輯器備份等）不比對，只提示。
    if (-not (Test-IsSourceFile $rel) -and ($listQ.Untracked.Contains($rel) -or $listA.Untracked.Contains($rel))) {
        Write-Warn "(1) 略過未追蹤的非原始碼檔：$rel（雜檔請刪除；若要 commit，先 git add 並同步兩邊）"
        continue
    }
    $qaChecked++
    if ($QaWhitelist.ContainsKey($rel)) {
        $pq = Join-Path $DirQ $rel
        $pa = Join-Path $DirA $rel
        if (-not (Test-Path -LiteralPath $pq -PathType Leaf) -or -not (Test-Path -LiteralPath $pa -PathType Leaf)) {
            Write-Err "(1) Q/A 白名單檔案缺檔：$rel"
            continue
        }
        $why = Test-PlatformNetIf (Get-NormalizedText $pq) (Get-NormalizedText $pa)
        if ($null -ne $why) {
            Write-Err "(1) Q/A 白名單檔案 $rel 超出允許的平台差異：$why"
        }
        continue
    }
    $r = Compare-Files $DirQ $DirA $rel
    switch -Wildcard ($r) {
        'same'      { }
        'missing-x' { Write-Err "(1) 只存在於 A、Q 沒有：$rel（Q 是正本：刪掉 A 的檔案，或先把它加進 Q）" }
        'missing-y' { Write-Err "(1) 只存在於 Q、A 沒有：$rel（從 Q 複製到 A，並確認 Android.mk 是否要加入）" }
        'diff@*'    { Write-Err "(1) Q/A 內容不一致：$rel（第 $($r.Substring(5)) 行起；以 Q 覆寫 A）" }
    }
}

# ── (2) Q 對 S3：server 實際編譯的指定檔案 ────────────────────────
foreach ($rel in $QsRequired) {
    $fold = ($QsIncludeCaseInsensitive -contains $rel)
    $r = Compare-Files $DirQ $DirS3 $rel -FoldIncludeCase:$fold
    switch -Wildcard ($r) {
        'same'         { }
        'missing-both' { Write-Err "(2) Q 與 S3 都找不到必要檔案：$rel" }
        'missing-x'    { Write-Err "(2) Q 缺少 server 使用的檔案：$rel" }
        'missing-y'    { Write-Err "(2) S3 缺少 server 使用的檔案：$rel" }
        'diff@*'       { Write-Err "(2) Q/S3 內容不一致：$rel（第 $($r.Substring(5)) 行起；server 編譯此檔，兩邊必須同步）" }
    }
}

# ── (3) 其他漂移：只警告 ───────────────────────────────────────
# Q 對 S3 的其餘檔案（S3 是 server 用的舊分支，多數檔案 server 不編譯）
$listS3 = Get-RelFiles $RelS3 @('src', 'enet')
$unionQs = New-Object 'System.Collections.Generic.SortedSet[string]' ([System.StringComparer]::Ordinal)
foreach ($f in $listQ.Files) { [void]$unionQs.Add($f) }
foreach ($f in $listS3.Files) { [void]$unionQs.Add($f) }
$qsDrift = New-Object 'System.Collections.Generic.List[string]'
foreach ($rel in $unionQs) {
    if ($QsRequired -contains $rel) { continue }
    $r = Compare-Files $DirQ $DirS3 $rel
    switch -Wildcard ($r) {
        'same'      { }
        'missing-x' { $qsDrift.Add("$rel（只在 S3）") }
        'missing-y' { $qsDrift.Add("$rel（只在 Q）") }
        'diff@*'    { $qsDrift.Add($rel) }
    }
}
if ($qsDrift.Count -gt 0) {
    Write-Warn "(3) Q/S3 其他漂移 $($qsDrift.Count) 檔（server 不編譯或目前已不同，僅提示）："
    foreach ($d in $qsDrift) { Write-Host "$Tag [WARN]    $d" }
}

# Q 對 A 的 nanors/（Android 經 rswrapper.c 編譯；目前一致，漂移先提示）
$nanorsQ = Get-RelFiles $RelQ @('nanors')
$nanorsA = Get-RelFiles $RelA @('nanors')
$unionN = New-Object 'System.Collections.Generic.SortedSet[string]' ([System.StringComparer]::Ordinal)
foreach ($f in $nanorsQ.Files) { [void]$unionN.Add($f) }
foreach ($f in $nanorsA.Files) { [void]$unionN.Add($f) }
foreach ($rel in $unionN) {
    $r = Compare-Files $DirQ $DirA $rel
    if ($r -ne 'same') {
        Write-Warn "(3) Q/A nanors 漂移：$rel（$r）"
    }
}

# ── 結果 ─────────────────────────────────────────────────────────
$summary = "(1) Q/A 檢查 $qaChecked 檔（白名單 $($QaWhitelist.Count)）、(2) Q/S3 必要 $($QsRequired.Count) 檔；錯誤 $($script:Errors)、警告 $($script:Warnings)"
if ($script:Errors -gt 0) {
    Write-Host "$Tag FAIL：$summary"
    exit 1
}
Write-Host "$Tag PASS：$summary"
exit 0
