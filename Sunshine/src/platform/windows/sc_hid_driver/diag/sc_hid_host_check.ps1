<#
.SYNOPSIS
  §SC-HID host 端健檢：驗 VipleStream 虛擬 Steam Controller（ROOT 節點 Viple\VID_28DE&PID_1302
  -> 子 HID 介面 28DE:1302）的 driver 版本、開啟、feature 通道與 host Steam 的握手證據。

.DESCRIPTION
  這支腳本把 §SC-HID Round 1（2026-09-02）在 host（.226）現場取證的步驟整理進版控，設計成由
  host worker 以 run-script 執行（也可在 host 上手動以管理員 PowerShell 跑）。輸出人可讀的
  段落，最後一行 RESULT:{json} 供 win-builder 解析。相容 Windows PowerShell 5.1 與 PowerShell 7。

  位置：Sunshine\src\platform\windows\sc_hid_driver\diag\（隨 repo 版控；repo 的 scripts\ 目錄
  整個是本機 gitignore，別放那裡）。host 黑板 scripts 的副本從這裡 bb_write + sync-scripts。

  段落：
    [1] 虛擬裝置：SetupDi 列舉 28DE:1302，attrs / caps / report 佈局、以 R+W 開啟（= server 的
        openHidByVidPid）。同時列出其他 28DE 裝置（host 若插著實體 SC 會在這裡露出）。
    [2] driver 統計（Feature 0x05，唯讀，不改任何狀態；driver >= 1.0.5.0 才宣告）：
        [0]=0x05 [1..2]=SET(0x01) 次數 LE16 [3..4]=GET(0x01) 次數 LE16 [5]=最後 0x01 feature op 的
        report id [6]=ring 深度 [7]=ResponsePending [8]=ResponseReady [9..10]=GET 事件 push 數
        [11..12]=有效回應交付數 [13..14]=GET 被閘控次數 [15..16]=全零回應被忽略數
        [17..18]=driver 版本（0x05,0x01）。舊 driver 不宣告 0x05 -> GET 失敗（supported=false）。
    [3] feature 通道（除非 -NoFeature／自動退場，見下）：
          GET 0x03（前，把 ring 內既有事件 pop 光）-> SET 0x01 [83 00] -> 每 ~1 ms GET 0x01，
          看到「err -> ok 轉折」即停，最多 MaxMs（鉗在 180 ms，< driver 閘控逾時 400 ms，確保
          看到的 ok 不是逾時退回 LastResponse）-> 立刻 GET 0x03（後）-> 再讀一次 0x05。判讀：
          - driver >= 1.0.5.0（RC2b 修好）：SET 在途、回應未到的 400 ms 內 GET 0x01 失敗
            （driver 回 STATUS_UNSUCCESSFUL -> user-mode ERROR_GEN_FAILURE(31)，與實體 Puck 相同；
            判讀邏輯接受任何非 0 error code，實際碼會印出來），GET 0x03 pop 出
            [03][op=2 SET][01][seq][len][01 83 00 ...]（query byte0 = report id）。0x03 自 1.0.5.0
            起是純事件，統計改走 0x05。
          - 舊 driver（<= 1.0.4.0）：GET 0x01 立刻 TRUE 但 buffer 原樣回來（transferred=0，
            這裡用 0xEE 哨兵偵測「echo」），GET 0x03 永遠 [03][00…] —— 這就是 F19。
          - **VipleStreamServer 服務 Running 時預設自動退為 -NoFeature**：SET 0x01 會跟進行中的
            Steam 握手搶同一個一次性回應暫存器，server 的 poll thread 也會先把事件 pop 走。
            確認沒有串流在跑時用 -ForceFeature 強制探測（warning 會附上 sunshine.log 看到的
            poll thread 狀態供判斷）。
    [4] PnP 節點 + driver store：ROOT 節點 status/problem/service/DriverVersion/INF、子 HID 節點、
        pnputil /enum-drivers 內所有 vipleschid.inf 套件（oemN.inf + 版本）。
    [5] 安裝目錄：5 個 driver 檔（大小/時間/簽章）、INF DriverVer vs 綁定版本、憑證信任狀態、
        舊路徑 sc_hid_driver\ 是否殘留、config\schid-deploy.log 末 5 行。
    [6] 服務 + sunshine.log 最近 [SC-HID] 行（區分「檔案不存在」「讀不到」「有 log 但無匹配行」）。
    [7] host Steam：是否在跑；controller.txt / console_log.txt 最近 N 行中含
        28de|1302|Read failure|couldn't get|zombie|Firmware 的行（握手失敗特徵：每秒
        "couldn't get controller details" + "Read failure" -> 10.8 s timeout -> zombie 重開）。
    [8] Install-VipleSCHid.ps1 -Diag（唯讀；除非 -SkipInstallerDiag）。

  host worker 走 run-script 時可用 COWORK_TASK_SPEC 覆寫參數，args.* 與頂層同名鍵皆接受（args 優先）：
    { op:"run-script", script:"sc_hid_host_check.ps1",
      args:{ dest, maxMs, steamLogLines, noFeature, forceFeature, skipInstallerDiag } }

.PARAMETER Dest            server 安裝目錄（預設 C:\Program Files\VipleStream-Server）
.PARAMETER MaxMs           GET 0x01 輪詢上限 ms（預設 180；上限鉗 180 = 低於 driver 閘控逾時 400 ms）
.PARAMETER SteamLogLines   Steam log 各檔掃描末尾行數（預設 200）
.PARAMETER NoFeature       不對虛擬裝置做 SET/GET（純唯讀；0x05 統計仍會讀）
.PARAMETER ForceFeature    服務 Running 時仍做 feature 探測（確認沒有串流在跑再用）
.PARAMETER SkipInstallerDiag 不呼叫 Install-VipleSCHid.ps1 -Diag
.PARAMETER Json            另存 JSON 檔

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File sc_hid_host_check.ps1
  pwsh sc_hid_host_check.ps1 -NoFeature -SteamLogLines 500
  pwsh sc_hid_host_check.ps1 -ForceFeature        # 部署後、沒有串流：驗 GATE-OK
#>
param(
  [string]$Dest = 'C:\Program Files\VipleStream-Server',
  [int]$MaxMs = 180,
  [int]$SteamLogLines = 200,
  [switch]$NoFeature,
  [switch]$ForceFeature,
  [switch]$SkipInstallerDiag,
  [string]$Json = ''
)
$ErrorActionPreference = 'Continue'
# [5] 的 Get-AuthenticodeSignature 與 Cert: 提供者都在這個模組。5.1 若繼承了 PowerShell 7 的
# PSModulePath（pwsh 型 worker 啟動 powershell.exe）會誤載 7 版同名模組而失敗 → 重設成 5.1 預設路徑再載。
try { Import-Module Microsoft.PowerShell.Security -ErrorAction Stop } catch { }
if (-not (Get-PSDrive Cert -ErrorAction SilentlyContinue) -and $PSVersionTable.PSVersion.Major -le 5) {
  $env:PSModulePath = "$env:ProgramFiles\WindowsPowerShell\Modules;$env:SystemRoot\system32\WindowsPowerShell\v1.0\Modules"
  try { Import-Module Microsoft.PowerShell.Security -ErrorAction Stop } catch { }
}

# JSON 布林／字串／數字都當布林看（"true"/"1"/"yes"/"on" 為真）。
function ConvertTo-FlagBool($v) {
  if ($null -eq $v) { return $false }
  if ($v -is [bool]) { return $v }
  $s = "$v".Trim().ToLowerInvariant()
  return ($s -in @('1', 'true', 'yes', 'y', 'on'))
}

# run-script 注入：COWORK_TASK_SPEC(JSON) 覆寫命名參數。args.* 優先，其次頂層同名鍵
#（host worker 的 dispatch 有時把 args 攤平到 spec 頂層；與 collect_server_stats.ps1 同慣例）。
if ($env:COWORK_TASK_SPEC) {
  try {
    $spec = $env:COWORK_TASK_SPEC | ConvertFrom-Json
    $specSources = @()
    if ($spec -and $spec.PSObject.Properties['args'] -and $null -ne $spec.args) { $specSources += $spec.args }
    if ($spec) { $specSources += $spec }
    function Get-SpecArg([string]$Name) {
      foreach ($srcObj in $specSources) {
        $prop = $srcObj.PSObject.Properties[$Name]
        if ($prop -and $null -ne $prop.Value -and "$($prop.Value)" -ne '') { return $prop.Value }
      }
      return $null
    }
    $v = Get-SpecArg 'dest';              if ($null -ne $v) { $Dest = [string]$v }
    $v = Get-SpecArg 'maxMs';             if ($null -ne $v) { $MaxMs = [int]$v }
    $v = Get-SpecArg 'steamLogLines';     if ($null -ne $v) { $SteamLogLines = [int]$v }
    $v = Get-SpecArg 'noFeature';         if (ConvertTo-FlagBool $v) { $NoFeature = $true }
    $v = Get-SpecArg 'forceFeature';      if (ConvertTo-FlagBool $v) { $ForceFeature = $true }
    $v = Get-SpecArg 'skipInstallerDiag'; if (ConvertTo-FlagBool $v) { $SkipInstallerDiag = $true }
  } catch { }
}
# 輪詢上限鉗 180 ms：低於 driver 的 GET(0x01) 閘控逾時 400 ms，這樣「輪詢期間看到 ok」只可能是
# 回應真的被交付（err -> ok 轉折），不會是逾時退回 LastResponse 的假 ok。
if ($MaxMs -gt 180) { $MaxMs = 180 }
if ($MaxMs -lt 5) { $MaxMs = 5 }

$src = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public class VIface {
  public int Index; public string Path; public int Vid; public int Pid; public int Rev;
  public int UsagePage; public int Usage; public int InLen; public int OutLen; public int FeatLen;
  public string Product = ""; public string Serial = "";
  public string InputIds = ""; public string OutputIds = ""; public string FeatureIds = "";
  public int OpenRwErr; public int OpenRoErr; public int CapsStatus;
}

// driver Feature 0x05 統計（driver >= 1.0.5.0）。Supported=false 表示 GET 0x05 失敗（舊 driver 不宣告）。
public class VStats05 {
  public bool Supported; public int Err; public string Raw = "";
  public int SetCount; public int GetCount; public int LastId; public int Ring; public bool Pending; public bool Ready;
  public int GetEvtPushed; public int Delivered; public int Gated; public int ZeroDropped; public int VerRaw;
  public string Summary = "";
}

public class VProbe {
  public int OpenErr;
  public List<string> Before = new List<string>(); public int BeforeGetErr; public VStats05 Before05;
  public bool SetOk; public int SetErr; public double SetMs;
  public int Tries; public int OkEcho; public int OkReal; public double FirstOkMs = -1; public string FirstOkResp = ""; public bool FirstOkIsType;
  public string LastOkResp = ""; public string ErrHist = ""; public int FirstErr; public double TotalMs;
  public bool Transition; public double TransitionMs = -1; public string TransitionResp = "";   // err -> ok 轉折（即停）
  public List<string> After = new List<string>(); public bool AfterSetEvent; public string AfterSetQuery = ""; public int AfterGetErr; public VStats05 After05;
}

public static class ScVHid {
  [StructLayout(LayoutKind.Sequential)] public struct DID { public uint cbSize; public Guid g; public uint Flags; public IntPtr Reserved; }
  [StructLayout(LayoutKind.Sequential)] public struct ATTR { public uint Size; public ushort VendorID; public ushort ProductID; public ushort VersionNumber; }
  [StructLayout(LayoutKind.Sequential)] public struct CAPS {
    public ushort Usage; public ushort UsagePage; public ushort InLen; public ushort OutLen; public ushort FeatLen;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst=17)] public ushort[] Reserved;
    public ushort NumLinkColl, NumInBtn, NumInVal, NumInIdx, NumOutBtn, NumOutVal, NumOutIdx, NumFtBtn, NumFtVal, NumFtIdx;
  }
  [DllImport("hid.dll")] static extern void HidD_GetHidGuid(out Guid g);
  [DllImport("hid.dll", SetLastError=true)] static extern bool HidD_GetAttributes(IntPtr h, ref ATTR a);
  [DllImport("hid.dll", SetLastError=true)] static extern bool HidD_GetPreparsedData(IntPtr h, out IntPtr pp);
  [DllImport("hid.dll")] static extern bool HidD_FreePreparsedData(IntPtr pp);
  [DllImport("hid.dll")] static extern int HidP_GetCaps(IntPtr pp, ref CAPS caps);
  [DllImport("hid.dll")] static extern int HidP_GetValueCaps(int type, IntPtr caps, ref ushort len, IntPtr pp);
  [DllImport("hid.dll")] static extern int HidP_GetButtonCaps(int type, IntPtr caps, ref ushort len, IntPtr pp);
  [DllImport("hid.dll", CharSet=CharSet.Unicode)] static extern bool HidD_GetProductString(IntPtr h, StringBuilder s, uint len);
  [DllImport("hid.dll", CharSet=CharSet.Unicode)] static extern bool HidD_GetSerialNumberString(IntPtr h, StringBuilder s, uint len);
  [DllImport("hid.dll", SetLastError=true)] static extern bool HidD_SetFeature(IntPtr h, byte[] b, uint len);
  [DllImport("hid.dll", SetLastError=true)] static extern bool HidD_GetFeature(IntPtr h, byte[] b, uint len);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode)] static extern IntPtr SetupDiGetClassDevsW(ref Guid g, IntPtr e, IntPtr p, uint f);
  [DllImport("setupapi.dll")] static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, uint i, ref DID did);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode)] static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr s, ref DID d, IntPtr det, uint sz, ref uint req, IntPtr dd);
  [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint ms);
  [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint ms);

  const uint DIGCF_PRESENT = 0x2, DIGCF_DEVICEINTERFACE = 0x10;
  const uint GR = 0x80000000, GW = 0x40000000, SHARE_RW = 0x3, OPEN_EXISTING = 3;
  const int HIDP_STATUS_SUCCESS = 0x110000;
  static readonly IntPtr INVALID = new IntPtr(-1);

  public static string Hex(byte[] b, int off, int n) {
    if (b == null || off >= b.Length) return "";
    if (off + n > b.Length) n = b.Length - off;
    if (n <= 0) return "";
    return BitConverter.ToString(b, off, n).Replace("-", " ");
  }
  static IntPtr Open(string path, uint access) { return CreateFileW(path, access, SHARE_RW, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero); }

  public static List<VIface> Enumerate(int vid) {
    List<VIface> list = new List<VIface>();
    Guid hg; HidD_GetHidGuid(out hg);
    IntPtr set = SetupDiGetClassDevsW(ref hg, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID) return list;
    try {
      DID did = new DID(); did.cbSize = (uint)Marshal.SizeOf(typeof(DID));
      for (uint i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref hg, i, ref did); i++) {
        uint req = 0;
        SetupDiGetDeviceInterfaceDetailW(set, ref did, IntPtr.Zero, 0, ref req, IntPtr.Zero);
        did.cbSize = (uint)Marshal.SizeOf(typeof(DID));
        if (req == 0) continue;
        IntPtr det = Marshal.AllocHGlobal((int)req);
        Marshal.WriteInt32(det, IntPtr.Size == 8 ? 8 : 6);
        string path = null;
        if (SetupDiGetDeviceInterfaceDetailW(set, ref did, det, req, ref req, IntPtr.Zero))
          path = Marshal.PtrToStringUni(new IntPtr((long)det + 4));
        Marshal.FreeHGlobal(det);
        if (path == null) continue;
        IntPtr h0 = Open(path, 0);
        if (h0 == INVALID) h0 = Open(path, GR);
        if (h0 == INVALID) continue;
        ATTR a = new ATTR(); a.Size = (uint)Marshal.SizeOf(typeof(ATTR));
        bool gA = HidD_GetAttributes(h0, ref a);
        CloseHandle(h0);
        if (!gA || a.VendorID != vid) continue;
        VIface f = new VIface();
        f.Index = list.Count; f.Path = path; f.Vid = a.VendorID; f.Pid = a.ProductID; f.Rev = a.VersionNumber;
        IntPtr hrw = Open(path, GR | GW);
        f.OpenRwErr = (hrw == INVALID) ? Marshal.GetLastWin32Error() : 0;
        IntPtr hro = Open(path, GR);
        f.OpenRoErr = (hro == INVALID) ? Marshal.GetLastWin32Error() : 0;
        IntPtr hc = (hrw != INVALID) ? hrw : hro;
        if (hc != INVALID) {
          IntPtr pp;
          if (HidD_GetPreparsedData(hc, out pp)) {
            CAPS c = new CAPS(); c.Reserved = new ushort[17];
            f.CapsStatus = HidP_GetCaps(pp, ref c);
            if (f.CapsStatus == HIDP_STATUS_SUCCESS) {
              f.UsagePage = c.UsagePage; f.Usage = c.Usage; f.InLen = c.InLen; f.OutLen = c.OutLen; f.FeatLen = c.FeatLen;
              f.InputIds   = ReportLayout(pp, 0, c.NumInVal,  c.NumInBtn);
              f.OutputIds  = ReportLayout(pp, 1, c.NumOutVal, c.NumOutBtn);
              f.FeatureIds = ReportLayout(pp, 2, c.NumFtVal,  c.NumFtBtn);
            }
            HidD_FreePreparsedData(pp);
          }
          StringBuilder sb = new StringBuilder(256);
          if (HidD_GetProductString(hc, sb, 512)) f.Product = sb.ToString();
          sb = new StringBuilder(256);
          if (HidD_GetSerialNumberString(hc, sb, 512)) f.Serial = sb.ToString();
        }
        if (hrw != INVALID) CloseHandle(hrw);
        if (hro != INVALID) CloseHandle(hro);
        list.Add(f);
      }
    } finally { SetupDiDestroyDeviceInfoList(set); }
    return list;
  }

  // 同 sc_hid_probe.ps1：raw offset 讀 HIDP_VALUE_CAPS / HIDP_BUTTON_CAPS（72 bytes）彙整每個 report id 的 byte 數。
  static string ReportLayout(IntPtr pp, int type, int nVal, int nBtn) {
    Dictionary<int,int> bits = new Dictionary<int,int>();
    List<int> order = new List<int>();
    if (nVal > 0) {
      ushort n = (ushort)nVal; IntPtr buf = Marshal.AllocHGlobal(72 * nVal);
      try {
        if (HidP_GetValueCaps(type, buf, ref n, pp) == HIDP_STATUS_SUCCESS) {
          for (int i = 0; i < n; i++) {
            IntPtr p = new IntPtr((long)buf + 72 * i);
            int rid = Marshal.ReadByte(p, 2);
            bool isRange = Marshal.ReadByte(p, 12) != 0;
            int bitSize = Marshal.ReadInt16(p, 18) & 0xFFFF;
            int cnt = Marshal.ReadInt16(p, 20) & 0xFFFF;
            int usages = 1;
            if (isRange) { int umin = Marshal.ReadInt16(p, 56) & 0xFFFF; int umax = Marshal.ReadInt16(p, 58) & 0xFFFF; usages = umax - umin + 1; if (usages < 1) usages = 1; }
            if (!bits.ContainsKey(rid)) { bits[rid] = 0; order.Add(rid); }
            bits[rid] += bitSize * cnt * usages;
          }
        }
      } finally { Marshal.FreeHGlobal(buf); }
    }
    if (nBtn > 0) {
      ushort n = (ushort)nBtn; IntPtr buf = Marshal.AllocHGlobal(72 * nBtn);
      try {
        if (HidP_GetButtonCaps(type, buf, ref n, pp) == HIDP_STATUS_SUCCESS) {
          for (int i = 0; i < n; i++) {
            IntPtr p = new IntPtr((long)buf + 72 * i);
            int rid = Marshal.ReadByte(p, 2);
            bool isRange = Marshal.ReadByte(p, 12) != 0;
            int usages = 1;
            if (isRange) { int umin = Marshal.ReadInt16(p, 56) & 0xFFFF; int umax = Marshal.ReadInt16(p, 58) & 0xFFFF; usages = umax - umin + 1; if (usages < 1) usages = 1; }
            if (!bits.ContainsKey(rid)) { bits[rid] = 0; order.Add(rid); }
            bits[rid] += usages;
          }
        }
      } finally { Marshal.FreeHGlobal(buf); }
    }
    StringBuilder sb = new StringBuilder();
    foreach (int rid in order) { if (sb.Length > 0) sb.Append(' '); sb.Append(String.Format("0x{0:X2}({1}B)", rid, (bits[rid] + 7) / 8)); }
    return sb.ToString();
  }

  // 解碼 driver 的 0x03 notify report：[03][op][reportId][seq][queryLen][query...]（純事件，driver 1.0.5.0 起不帶統計）。
  // query 是完整 feature report，byte0 就是 report id（Steam SET 0x01 -> query=[01 83 00 ...]）。
  static string DecodeEvt(byte[] b) {
    string op = b[1] == 0 ? "NONE" : (b[1] == 1 ? "GET" : (b[1] == 2 ? "SET" : String.Format("op{0}", b[1])));
    int qn = b[4];
    if (b[1] == 0) return "op=NONE (ring empty)";
    return String.Format("op={0} reportId=0x{1:X2} seq={2} queryLen={3} query=[{4}] (query byte0 = report id)", op, b[2], b[3], qn, Hex(b, 5, Math.Min(qn, 16)));
  }

  // driver Feature 0x05 統計（唯讀；佈局見腳本 .DESCRIPTION [2]）。
  public static VStats05 ReadStats05(IntPtr h, int len) {
    VStats05 s = new VStats05();
    if (len < 19) len = 64;
    byte[] b = new byte[len]; b[0] = 0x05;
    if (!HidD_GetFeature(h, b, (uint)len)) {
      s.Err = Marshal.GetLastWin32Error();
      s.Summary = String.Format("0x05 GET failed err={0} (driver < 1.0.5.0 does not declare Feature 0x05)", s.Err);
      return s;
    }
    s.Raw = Hex(b, 0, 20);
    if (b[0] != 0x05) { s.Summary = "0x05 GET returned unexpected report id: " + s.Raw; return s; }
    s.Supported = true;
    s.SetCount = b[1] | (b[2] << 8);
    s.GetCount = b[3] | (b[4] << 8);
    s.LastId = b[5];
    s.Ring = b[6];
    s.Pending = b[7] != 0;
    s.Ready = b[8] != 0;
    s.GetEvtPushed = b[9] | (b[10] << 8);
    s.Delivered = b[11] | (b[12] << 8);
    s.Gated = b[13] | (b[14] << 8);
    s.ZeroDropped = b[15] | (b[16] << 8);
    s.VerRaw = b[17] | (b[18] << 8);
    s.Summary = String.Format("drvSet={0} drvGet01={1} lastId=0x{2:X2} ring={3} pending={4} ready={5} getEvtPushed={6} delivered={7} gated={8} zeroDropped={9} drvVer=0x{10:X4}",
      s.SetCount, s.GetCount, s.LastId, s.Ring, s.Pending ? 1 : 0, s.Ready ? 1 : 0, s.GetEvtPushed, s.Delivered, s.Gated, s.ZeroDropped, s.VerRaw);
    return s;
  }

  // 獨立開 handle 讀 0x05（[2] 段落用；R+W 開不了就退 R）。
  public static VStats05 ReadStats05Path(string path, int len) {
    IntPtr h = Open(path, GR | GW);
    if (h == INVALID) h = Open(path, GR);
    if (h == INVALID) { VStats05 s = new VStats05(); s.Err = Marshal.GetLastWin32Error(); s.Summary = String.Format("open failed err={0}", s.Err); return s; }
    try { return ReadStats05(h, len); } finally { CloseHandle(h); }
  }

  // pop 0x03 直到 op=NONE（最多 17 次，ring 深度 16）
  static void Drain03(IntPtr h, int len, List<string> outList, ref int getErr, ref bool sawSet, ref string setQuery) {
    for (int i = 0; i < 17; i++) {
      byte[] b = new byte[len]; b[0] = 0x03;
      if (!HidD_GetFeature(h, b, (uint)len)) { getErr = Marshal.GetLastWin32Error(); outList.Add(String.Format("GET 0x03 failed err={0}", getErr)); return; }
      string d = DecodeEvt(b);
      if (b[1] == 0) { if (i == 0) outList.Add(d); return; }
      outList.Add(d);
      if (b[1] == 2 && b[2] == 0x01) { sawSet = true; setQuery = Hex(b, 5, Math.Min((int)b[4], 8)); }
    }
  }

  public static VProbe FeatureProbe(VIface f, int maxMs) {
    VProbe r = new VProbe();
    int len = f.FeatLen > 0 ? f.FeatLen : 64;
    IntPtr h = Open(f.Path, GR | GW);
    if (h == INVALID) { r.OpenErr = Marshal.GetLastWin32Error(); return r; }
    timeBeginPeriod(1);
    try {
      bool dummySet = false; string dummyQ = "";
      Drain03(h, len, r.Before, ref r.BeforeGetErr, ref dummySet, ref dummyQ);
      r.Before05 = ReadStats05(h, len);

      Stopwatch sw = Stopwatch.StartNew();
      byte[] q = new byte[len]; q[0] = 0x01; q[1] = 0x83; q[2] = 0x00;
      r.SetOk = HidD_SetFeature(h, q, (uint)len);
      r.SetErr = r.SetOk ? 0 : Marshal.GetLastWin32Error();
      r.SetMs = sw.Elapsed.TotalMilliseconds;

      Dictionary<int,int> errs = new Dictionary<int,int>(); List<int> order = new List<int>();
      bool inErr = false;
      while (sw.ElapsedMilliseconds <= maxMs) {
        byte[] b = new byte[len]; b[0] = 0x01;
        for (int i = 1; i < len; i++) b[i] = 0xEE;   // 哨兵：driver 沒寫任何 byte 就會原樣回來（F19 的 echo）
        r.Tries++;
        if (HidD_GetFeature(h, b, (uint)len)) {
          bool echo = true;
          for (int i = 1; i < len; i++) if (b[i] != 0xEE) { echo = false; break; }
          if (echo) r.OkEcho++; else r.OkReal++;
          string resp = Hex(b, 0, 16) + (echo ? " (echo)" : "");
          if (r.FirstOkMs < 0) { r.FirstOkMs = sw.Elapsed.TotalMilliseconds; r.FirstOkResp = resp; r.FirstOkIsType = (!echo && b[1] == 0x83); }
          r.LastOkResp = resp;
          if (inErr) {
            // err -> ok 轉折：回應已交付（或閘控解除）。即停，馬上去 drain 0x03，縮短與 server poll thread 的競賽窗。
            r.Transition = true; r.TransitionMs = sw.Elapsed.TotalMilliseconds; r.TransitionResp = resp;
            break;
          }
        } else {
          int e = Marshal.GetLastWin32Error();
          if (!errs.ContainsKey(e)) { errs[e] = 0; order.Add(e); }
          errs[e]++;
          if (r.FirstErr == 0) r.FirstErr = e;
          inErr = true;
        }
        Thread.Sleep(1);
      }
      r.TotalMs = sw.Elapsed.TotalMilliseconds;

      // 輪詢一停就 drain 0x03（在整理字串之前），再讀一次 0x05。
      Drain03(h, len, r.After, ref r.AfterGetErr, ref r.AfterSetEvent, ref r.AfterSetQuery);
      r.After05 = ReadStats05(h, len);

      StringBuilder sb = new StringBuilder();
      foreach (int e in order) { if (sb.Length > 0) sb.Append(','); sb.Append(String.Format("err{0}x{1}", e, errs[e])); }
      r.ErrHist = sb.ToString();
    } finally { timeEndPeriod(1); CloseHandle(h); }
    return r;
  }
}
'@
Add-Type -TypeDefinition $src -Language CSharp

function Get-NodeProp([string]$InstanceId, [string]$Key) {
  $d = (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $Key -ErrorAction SilentlyContinue).Data
  if ($null -eq $d) { return '' }
  return "$($d -join ',')"
}
function Get-InfDriverVer([string]$Path) {
  try {
    $m = [regex]::Match((Get-Content -LiteralPath $Path -Raw -ErrorAction Stop), '(?im)^\s*DriverVer\s*=\s*[^,\r\n]*,\s*([0-9]+(?:\.[0-9]+){1,3})')
    if ($m.Success) { return $m.Groups[1].Value }
  } catch { }
  return ''
}
# VStats05 -> 可序列化的 ordered hashtable
function ConvertTo-Stats05Obj($s) {
  if ($null -eq $s) { return $null }
  if (-not $s.Supported) { return [ordered]@{ supported = $false; err = $s.Err; summary = $s.Summary } }
  return [ordered]@{ supported = $true; drvSet = $s.SetCount; drvGet01 = $s.GetCount; lastId = ('0x{0:X2}' -f $s.LastId); ring = $s.Ring
                     pending = $s.Pending; ready = $s.Ready; getEvtPushed = $s.GetEvtPushed; delivered = $s.Delivered
                     gated = $s.Gated; zeroDropped = $s.ZeroDropped; drvVer = ('0x{0:X4}' -f $s.VerRaw); raw = $s.Raw; summary = $s.Summary }
}
# sunshine.log：區分「檔案不存在」「讀不到（權限／鎖）」「讀到了」。
function Read-SunshineLog([string]$Path) {
  $r = @{ state = 'missing'; lines = @() }
  if (-not (Test-Path -LiteralPath $Path)) { return $r }
  try {
    $r.lines = @(Get-Content -LiteralPath $Path -Tail 20000 -ErrorAction Stop)
    $r.state = 'ok'
  } catch {
    $r.state = "unreadable: $($_.Exception.Message)"
  }
  return $r
}

$hwid = 'Viple\VID_28DE&PID_1302'
$R = [ordered]@{ host = $env:COMPUTERNAME; time = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'); ps = $PSVersionTable.PSVersion.ToString() }
Write-Host ("=== SC-HID host check  {0}  host={1}  PS {2}  admin={3} ===" -f $R.time, $R.host, $R.ps,
  ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))

# 服務狀態 + sunshine.log 先讀一次（[3] 的自動退場 warning 與 [6] 共用）。
$svcObj = Get-Service VipleStreamServer -ErrorAction SilentlyContinue
$svcRunning = ($svcObj -and $svcObj.Status -eq 'Running')
$slogPath = Join-Path $Dest 'config\sunshine.log'
$slog = Read-SunshineLog $slogPath
$scLines = @()
if ($slog.state -eq 'ok') { $scLines = @($slog.lines | Select-String -SimpleMatch '[SC-HID]' | ForEach-Object { $_.Line }) }
# poll thread 狀態（只看 [SC-HID] 行的最後一個 started/exited 標記）：started = 有 session 在跑（串流中）。
$pollThreadState = 'none'
$lastMarker = $scLines | Where-Object { $_ -match 'Polling thread started|Polling thread exited' } | Select-Object -Last 1
if ($lastMarker) { $pollThreadState = $(if ($lastMarker -match 'Polling thread exited') { 'exited' } else { 'started' }) }

# ---- [1] 虛擬裝置 ----
$all = [ScVHid]::Enumerate(0x28DE)
$virt = @($all | Where-Object { $_.Pid -eq 0x1302 })
$others = @($all | Where-Object { $_.Pid -ne 0x1302 })
Write-Host ("[1] 28DE HID interfaces: {0} total, {1} with PID 0x1302 (virtual SC), {2} other" -f $all.Count, $virt.Count, $others.Count)
foreach ($f in $all) {
  $rw = if ($f.OpenRwErr -eq 0) { 'OK' } else { "FAIL err=$($f.OpenRwErr)" }
  $ro = if ($f.OpenRoErr -eq 0) { 'OK' } else { "FAIL err=$($f.OpenRoErr)" }
  Write-Host ("  #{0} PID=0x{1:X4} rev=0x{2:X4} UP=0x{3:X4}/U=0x{4:X4} in={5} out={6} feat={7} open(R+W)={8} open(R)={9} product='{10}' serial='{11}'" -f $f.Index, $f.Pid, $f.Rev, $f.UsagePage, $f.Usage, $f.InLen, $f.OutLen, $f.FeatLen, $rw, $ro, $f.Product, $f.Serial)
  Write-Host ("       path={0}" -f $f.Path)
  Write-Host ("       INPUT: {0} | OUTPUT: {1} | FEATURE: {2}" -f $f.InputIds, $f.OutputIds, $f.FeatureIds)
}
$dev = $null
if ($virt.Count -gt 0) { $dev = $virt[0] }
$R.deviceFound = ($null -ne $dev)
if ($dev) {
  $R.devicePath = $dev.Path; $R.deviceRev = ('0x{0:X4}' -f $dev.Rev); $R.openRwErr = $dev.OpenRwErr
  $R.inputIds = $dev.InputIds; $R.outputIds = $dev.OutputIds; $R.featureIds = $dev.FeatureIds
  $R.declares42 = ($dev.InputIds -match '0x42\(')
  $R.declares05 = ($dev.FeatureIds -match '0x05\(')
} else {
  Write-Host '  !! no 28DE:1302 interface -> server would log "Virtual Steam Controller device not found" (G0 fails)'
}

# ---- [2] driver 統計（Feature 0x05，唯讀） ----
$stats05 = $null
if (-not $dev) {
  Write-Host '[2] driver stats (Feature 0x05) skipped (no device)'
} elseif ($dev.OpenRwErr -ne 0 -and $dev.OpenRoErr -ne 0) {
  Write-Host ("[2] driver stats (Feature 0x05) skipped (cannot open, R+W err={0} R err={1})" -f $dev.OpenRwErr, $dev.OpenRoErr)
} else {
  $stats05 = [ScVHid]::ReadStats05Path($dev.Path, $(if ($dev.FeatLen -gt 0) { $dev.FeatLen } else { 64 }))
  Write-Host ("[2] driver stats (Feature 0x05): {0}" -f $stats05.Summary)
  if ($stats05.Supported) {
    Write-Host ("    raw: {0}" -f $stats05.Raw)
    if ($stats05.SetCount -eq 0) { Write-Host '    note: drvSet=0 -> no Steam SET_FEATURE(0x01) has reached the driver since the device context was created (Steam not polling it, or handshake never started)' }
    if ($stats05.Pending -and -not $stats05.Ready) { Write-Host '    note: a SET is pending with no response delivered yet (GET 0x01 is being gated right now)' }
  } else {
    Write-Host '    -> old driver (< 1.0.5.0) or feature channel broken; see [3]/[5] for version evidence'
  }
  $R.driverStats = ConvertTo-Stats05Obj $stats05
}

# ---- [3] feature 通道 ----
$autoNoFeature = $false
if (-not $NoFeature -and -not $ForceFeature -and $svcRunning) { $autoNoFeature = $true; $NoFeature = $true }
if ($NoFeature -and $ForceFeature -and -not $autoNoFeature) { Write-Host 'WARNING: both -NoFeature and -ForceFeature given; -NoFeature wins (no feature probe).' }
$R.featureProbeMode = $(if ($autoNoFeature) { 'auto-disabled(service running)' } elseif ($NoFeature) { 'disabled(-NoFeature)' } elseif ($ForceFeature) { 'forced' } else { 'enabled' })
if ($autoNoFeature) {
  Write-Host '[3] feature channel probe AUTO-DISABLED: VipleStreamServer is Running (a stream may be active).'
  Write-Host '    WARNING: SET 0x01 would compete with an in-flight Steam handshake for the one-shot response register,'
  Write-Host '             and the running server poll thread pops ring events before we can. Pass -ForceFeature'
  Write-Host '             (or spec args.forceFeature=true) only when no stream is running.'
  Write-Host ("    hint: sunshine.log {0}; last [SC-HID] poll-thread marker = {1} ({2})" -f $slog.state, $pollThreadState,
    $(if ($pollThreadState -eq 'started') { 'a session/poll thread looks ACTIVE -> keep feature probe off' } elseif ($pollThreadState -eq 'exited') { 'last session ended -> -ForceFeature is probably safe' } else { 'no session marker in tail' }))
} elseif ($NoFeature) {
  Write-Host '[3] feature channel probe skipped (-NoFeature)'
} elseif (-not $dev) {
  Write-Host '[3] feature channel probe skipped (no device)'
} elseif ($dev.OpenRwErr -ne 0) {
  Write-Host ("[3] feature channel probe skipped (cannot open R+W, err={0})" -f $dev.OpenRwErr)
} else {
  Write-Host ("[3] Feature channel: GET 0x03 (drain) -> SET 0x01 [83 00] -> poll GET 0x01 every ~1 ms, stop at first err->ok transition or {0} ms -> GET 0x03 (drain) -> 0x05" -f $MaxMs)
  if ($svcRunning) { Write-Host '    note: -ForceFeature with VipleStreamServer Running - its poll thread may pop the SET event from the 0x03 ring before we do.' }
  $p = [ScVHid]::FeatureProbe($dev, $MaxMs)
  if ($p.OpenErr -ne 0) { Write-Host ("  open failed err={0}" -f $p.OpenErr) }
  else {
    Write-Host ("  before: {0}" -f ($p.Before -join ' ; '))
    Write-Host ("  stats05 before: {0}" -f $p.Before05.Summary)
    Write-Host ("  SET 0x01 [83 00]: {0} err={1} ({2:F1} ms)" -f $(if ($p.SetOk) { 'TRUE' } else { 'FALSE' }), $p.SetErr, $p.SetMs)
    Write-Host ("  GET 0x01 x{0} over {1:F0} ms: okReal={2} okEcho={3} errors=[{4}] firstOk@{5:F1}ms={6} firstErr={7}" -f $p.Tries, $p.TotalMs, $p.OkReal, $p.OkEcho, $p.ErrHist, $p.FirstOkMs, $p.FirstOkResp, $p.FirstErr)
    if ($p.Transition) { Write-Host ("  err->ok transition @{0:F1} ms (stopped polling): {1}" -f $p.TransitionMs, $p.TransitionResp) }
    if ($p.LastOkResp) { Write-Host ("  last GET 0x01 ok: {0}" -f $p.LastOkResp) }
    Write-Host ("  after : {0}" -f ($p.After -join ' ; '))
    Write-Host ("  stats05 after : {0}" -f $p.After05.Summary)
    $drvSetDelta = $null
    if ($p.Before05.Supported -and $p.After05.Supported) { $drvSetDelta = $p.After05.SetCount - $p.Before05.SetCount }
    $verdict = ''
    if ($p.OkEcho -gt 0 -and $p.OkReal -eq 0 -and -not $p.AfterSetEvent -and $p.FirstErr -eq 0) {
      $verdict = 'OLD-DRIVER-ECHO: GET 0x01 returns the caller buffer untouched and no SET event reached the ring -> feature handler not parsing (RC2b, driver <= 1.0.4.0)'
    } elseif ($p.AfterSetEvent -and $p.FirstErr -ne 0 -and $p.Transition) {
      $verdict = ("GATE-OK-RESPONSE-DELIVERED: SET event in ring (query {0}), GET 0x01 gated with err{1} then succeeded @{2:F1} ms (a response was delivered during the poll window)" -f $p.AfterSetQuery, $p.FirstErr, $p.TransitionMs)
    } elseif ($p.AfterSetEvent -and $p.FirstErr -ne 0) {
      $verdict = ("GATE-OK: SET event in ring (query {0}) and GET 0x01 fails with err{1} while pending -> driver >= 1.0.5.0 semantics" -f $p.AfterSetQuery, $p.FirstErr)
    } elseif ($p.AfterSetEvent) {
      $verdict = 'RING-OK-NO-GATE: SET event reached the ring but GET 0x01 never failed while pending (gate not active, or a response was delivered instantly)'
    } elseif ($p.FirstErr -ne 0 -and $svcRunning) {
      $verdict = ("GATE-SEEN-EVENT-CONSUMED: GET 0x01 failed with err{0} while pending; SET event not in ring (probably popped by the running server poll thread)" -f $p.FirstErr)
    } elseif ($p.FirstErr -ne 0) {
      $verdict = ("GATE-OK-BUT-NO-SET-EVENT: GET 0x01 gated with err{0} (SET handler ran) but no SET event in the 0x03 ring and the server is NOT running -> event push/ring path broken in the driver" -f $p.FirstErr)
    } else {
      $verdict = 'UNCLEAR: see raw lines above'
    }
    if ($null -ne $drvSetDelta) {
      if ($drvSetDelta -ge 1) { $verdict += (" [0x05 drvSet +{0}: SET ioctl reached the driver]" -f $drvSetDelta) }
      else { $verdict += ' [0x05 drvSet unchanged: SET ioctl did NOT reach the driver handler]' }
    }
    Write-Host ("  verdict: {0}" -f $verdict)
    $R.feature = [ordered]@{ setOk=$p.SetOk; setErr=$p.SetErr; tries=$p.Tries; totalMs=[math]::Round($p.TotalMs,1); okReal=$p.OkReal; okEcho=$p.OkEcho; errHist=$p.ErrHist; firstErr=$p.FirstErr
                             firstOkMs=[math]::Round($p.FirstOkMs,1); firstOkResp=$p.FirstOkResp; firstOkIsType83=$p.FirstOkIsType
                             transition=$p.Transition; transitionMs=[math]::Round($p.TransitionMs,1); transitionResp=$p.TransitionResp
                             ringSetEvent=$p.AfterSetEvent; ringSetQuery=$p.AfterSetQuery; drvSetDelta=$drvSetDelta
                             stats05Before=(ConvertTo-Stats05Obj $p.Before05); stats05After=(ConvertTo-Stats05Obj $p.After05)
                             before=@($p.Before); after=@($p.After); verdict=$verdict }
  }
}

# ---- [4] PnP 節點 + driver store ----
Write-Host '[4] PnP node / driver store'
$node = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'ROOT\*' } | Where-Object {
  $h = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction SilentlyContinue).Data
  $h -contains $hwid
} | Select-Object -First 1
if ($node) {
  $svc = Get-NodeProp $node.InstanceId 'DEVPKEY_Device_Service'
  $drvVer = Get-NodeProp $node.InstanceId 'DEVPKEY_Device_DriverVersion'
  $drvDate = Get-NodeProp $node.InstanceId 'DEVPKEY_Device_DriverDate'
  $drvInf = Get-NodeProp $node.InstanceId 'DEVPKEY_Device_DriverInfPath'
  $prob = Get-NodeProp $node.InstanceId 'DEVPKEY_Device_ProblemCode'
  $lower = Get-NodeProp $node.InstanceId 'DEVPKEY_Device_LowerFilters'
  Write-Host ("  root node: {0} status={1} problem={2} service={3} lowerFilters={4} driverVer={5} driverDate={6} inf={7}" -f $node.InstanceId, $node.Status, $prob, $svc, $lower, $drvVer, $drvDate, $drvInf)
  $R.node = [ordered]@{ instanceId=$node.InstanceId; status="$($node.Status)"; problem=$prob; service=$svc; lowerFilters=$lower; driverVersion=$drvVer; driverDate=$drvDate; inf=$drvInf }
  $R.bound = (($node.Status -eq 'OK') -and ($svc -ieq 'MsHidUmdf'))
} else {
  Write-Host "  root node with HWID ${hwid}: NOT PRESENT"
  $R.node = $null; $R.bound = $false
}
$children = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'HID\VID_28DE&PID_1302*' })
foreach ($c in $children) { Write-Host ("  child HID node: {0} status={1} class={2} name='{3}'" -f $c.InstanceId, $c.Status, $c.Class, $c.FriendlyName) }
$R.childNodes = @($children | ForEach-Object { "$($_.InstanceId) [$($_.Status)]" })
$legacy = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'ROOT\HIDCLASS\*' -or $_.InstanceId -like 'HID\STEAMCONTROLLER*' })
foreach ($c in $legacy) { Write-Host ("  legacy/odd node: {0} status={1} name='{2}'" -f $c.InstanceId, $c.Status, $c.FriendlyName) }

$pkgs = @()
try {
  $enum = (& pnputil /enum-drivers 2>&1 | ForEach-Object { "$_" }) -join "`n"
  foreach ($blk in ($enum -split "(?m)^\s*$")) {
    if ($blk -match '(?i)vipleschid\.inf') {
      $oem = if ($blk -match '(?i)(oem\d+\.inf)') { $matches[1] } else { '?' }
      $ver = if ($blk -match '(\d{1,2}/\d{1,2}/\d{4})\s+(\d+\.\d+\.\d+\.\d+)') { "$($matches[2]) ($($matches[1]))" } elseif ($blk -match '(\d+\.\d+\.\d+\.\d+)') { $matches[1] } else { '?' }
      Write-Host ("  driver store: {0} version={1}" -f $oem, $ver)
      $pkgs += "$oem $ver"
    }
  }
  if ($pkgs.Count -eq 0) { Write-Host '  driver store: no vipleschid.inf package' }
} catch { Write-Host "  pnputil failed: $($_.Exception.Message)" }
$R.driverStore = $pkgs

# ---- [5] 安裝目錄 ----
Write-Host "[5] Install dir: $Dest"
$files = [ordered]@{}
foreach ($f in @('VipleSCHid_Driver.dll', 'VipleSCHid.inf', 'VipleSCHid.cat', 'VipleSCHid_SelfSign.cer', 'Install-VipleSCHid.ps1')) {
  $p = Join-Path $Dest $f
  if (Test-Path $p) {
    $it = Get-Item $p
    $extra = ''
    if ($f -like '*.dll' -or $f -like '*.cat') {
      try { $sig = Get-AuthenticodeSignature $p; $extra = " sig=$($sig.Status) signer='$($sig.SignerCertificate.Subject)'" } catch { }
      if ($f -like '*.dll') { $extra += " fileVer=$($it.VersionInfo.FileVersion)" }
    }
    if ($f -like '*.inf') { $extra = " DriverVer=$(Get-InfDriverVer $p)" }
    if ($f -like '*.cer') {
      try {
        $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($p)
        $inTP = @(Get-ChildItem Cert:\LocalMachine\TrustedPublisher -ErrorAction SilentlyContinue | Where-Object { $_.Thumbprint -eq $cert.Thumbprint }).Count -gt 0
        $inRoot = @(Get-ChildItem Cert:\LocalMachine\Root -ErrorAction SilentlyContinue | Where-Object { $_.Thumbprint -eq $cert.Thumbprint }).Count -gt 0
        $extra = " thumb=$($cert.Thumbprint) inRoot=$inRoot inTrustedPublisher=$inTP"
      } catch { }
    }
    Write-Host ("  {0,-26} {1,8} bytes  {2:yyyy-MM-dd HH:mm:ss}{3}" -f $f, $it.Length, $it.LastWriteTime, $extra)
    $files[$f] = [ordered]@{ size=$it.Length; mtime=$it.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'); info=$extra.Trim() }
  } else {
    Write-Host ("  {0,-26} MISSING" -f $f)
    $files[$f] = $null
  }
}
$R.files = $files
$infVer = Get-InfDriverVer (Join-Path $Dest 'VipleSCHid.inf')
$R.infVersion = $infVer
$R.driverVersion = $(if ($R.node) { $R.node.driverVersion } else { '' })
$R.versionMatch = $(if ($infVer -and $R.driverVersion) { $infVer -eq $R.driverVersion } else { 'unknown' })
Write-Host ("  INF DriverVer={0} vs bound driverVersion={1} -> versionMatch={2}" -f $infVer, $R.driverVersion, $R.versionMatch)
if (Test-Path (Join-Path $Dest 'sc_hid_driver')) { Write-Host '  note: legacy sub-dir sc_hid_driver\ exists (old tryInstallDriver() lookup path)' }
$prevRoot = Join-Path $Dest 'sc_hid_driver_prev'
if (Test-Path $prevRoot) {
  $prevVers = @(Get-ChildItem $prevRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
  Write-Host ("  backup dir sc_hid_driver_prev\ present (deploy_from_release.ps1); versions: {0}" -f $(if ($prevVers.Count -gt 0) { $prevVers -join ', ' } else { '(flat/legacy layout)' }))
  $R.backupVersions = $prevVers
}
$dlog = Join-Path $Dest 'config\schid-deploy.log'
if (Test-Path $dlog) {
  Write-Host '  config\schid-deploy.log (last 5):'
  Get-Content $dlog -Tail 5 | ForEach-Object { Write-Host "    $_" }
  $R.deployLogTail = @(Get-Content $dlog -Tail 5)
}

# ---- [6] 服務 + sunshine.log ----
$proc = Get-Process viplestream-server -ErrorAction SilentlyContinue
Write-Host ("[6] service VipleStreamServer: {0}; viplestream-server.exe PID: {1}" -f $(if ($svcObj) { $svcObj.Status } else { 'not installed' }), $(if ($proc) { ($proc | ForEach-Object { $_.Id }) -join ',' } else { '-' }))
$R.serviceStatus = $(if ($svcObj) { "$($svcObj.Status)" } else { 'missing' })
$R.sunshineLogState = $slog.state
$R.pollThreadState = $pollThreadState
if ($slog.state -eq 'ok') {
  if ($scLines.Count -eq 0) {
    Write-Host ("  sunshine.log readable ({0} line(s) tailed) but NO [SC-HID] line in the last 20000 -> server never started the SC-HID poll thread in this window (no session yet, or SC-HID disabled)" -f $slog.lines.Count)
  } else {
    $sc = @($scLines | Select-Object -Last 15)
    Write-Host ("  sunshine.log last [SC-HID] lines ({0} of {1} matching; poll thread marker={2}):" -f $sc.Count, $scLines.Count, $pollThreadState)
    foreach ($l in $sc) { Write-Host "    $l" }
    $R.sunshineScHidTail = $sc
  }
  $R.sunshineScHidCount = $scLines.Count
} elseif ($slog.state -eq 'missing') {
  Write-Host "  sunshine.log NOT FOUND at $slogPath (server never ran from this Dest, or log path differs)"
} else {
  Write-Host "  sunshine.log UNREADABLE ($slogPath): $($slog.state)"
}

# ---- [7] host Steam ----
$steam = Get-Process steam -ErrorAction SilentlyContinue
Write-Host ("[7] Steam on host: {0}" -f $(if ($steam) { 'running (PID ' + (($steam | ForEach-Object { $_.Id }) -join ',') + ')' } else { 'not running' }))
$R.steamRunning = [bool]$steam
$steamDir = $null
try { $steamDir = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -ErrorAction Stop).InstallPath } catch { }
if (-not $steamDir) { $steamDir = 'C:\Program Files (x86)\Steam' }
$pat = '(?i)28de|1302|Read failure|couldn''t get|zombie|Firmware'
$hits = [ordered]@{}
foreach ($lf in @('controller.txt', 'console_log.txt')) {
  $lp = Join-Path $steamDir "logs\$lf"
  if (-not (Test-Path $lp)) { Write-Host "  ${lf}: not found ($lp)"; continue }
  $m = @(Get-Content $lp -Tail $SteamLogLines -ErrorAction SilentlyContinue | Where-Object { $_ -match $pat })
  Write-Host ("  {0}: {1} matching line(s) in last {2}" -f $lf, $m.Count, $SteamLogLines)
  foreach ($l in ($m | Select-Object -Last 40)) { Write-Host "    $l" }
  $hits[$lf] = @($m | Select-Object -Last 40)
}
$R.steamLogHits = $hits
$allHits = @($hits.Values | ForEach-Object { $_ })
$R.steamReadFailure = @($allHits | Where-Object { $_ -match '(?i)Read failure|couldn''t get controller details' }).Count
$R.steamZombie = @($allHits | Where-Object { $_ -match '(?i)zombie' }).Count

# ---- [8] Install-VipleSCHid.ps1 -Diag ----
$inst = Join-Path $Dest 'Install-VipleSCHid.ps1'
if ($SkipInstallerDiag) { Write-Host '[8] installer -Diag skipped' }
elseif (-not (Test-Path $inst)) { Write-Host '[8] installer -Diag skipped (Install-VipleSCHid.ps1 missing)' }
else {
  Write-Host '[8] Install-VipleSCHid.ps1 -Diag:'
  try { & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $inst -Diag 2>&1 | ForEach-Object { Write-Host "    $_" } } catch { Write-Host "    failed: $($_.Exception.Message)" }
}

# 注意：PowerShell 變數名不分大小寫，$json 會跟參數 $Json 撞名，所以用 $jsonText。
$jsonText = $R | ConvertTo-Json -Depth 8 -Compress
if ($Json) { $R | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Json -Encoding UTF8; Write-Host "  json -> $Json" }
Write-Host ('RESULT:' + $jsonText)
