<#
.SYNOPSIS
  §SC-HID client 端探測：列舉實體 Steam Controller（VID 0x28DE）的 HID 介面、report 佈局、
  N 秒 report id 直方圖、以及 feature 往返（SET 0x01 -> 每 ~1 ms 輪詢 GET 0x01）時序量測。

.DESCRIPTION
  這支腳本把 §SC-HID Round 1（2026-09-02）用來定案根因的硬體探測整理進版控，跑在
  「接著實體 Steam Controller 的 client 機」上（Windows；不需管理員）。它回答四個問題：

    1. 機器上有哪些 0x28DE 介面？（USB 直連 PID 0x1302；Puck 無線接收器 PID 0x1304 會
       露出 4 個 slot vendor 介面 MI_02~05 Col03 + 1 個 dongle 管理介面 MI_06）
    2. 每個介面的 HID 描述符宣告了哪些 report id / 長度？（HidP_GetValueCaps + ButtonCaps）
    3. 控制器醒著時實際吐什麼？（-Seconds N 讀取直方圖；Puck slot 是 ~266 Hz 的 0x42，
       不是 client 舊碼硬編的 0x45；另有 0x43 電量、0x7B 遙測、0x44 偶發）
    4. feature 回應的時序語義：SET 0x01 [type] 之後回應只在 13~21 ms 內一次性出現在
       GET 0x01，其他時間 GET 回 ERROR_GEN_FAILURE(31)；回應必須以 resp[1]==type 比對
       （會撿到未經請求的 0x87 ack）。-LegacyCheck 另外模擬舊 client 的
       「SET -> 等 20 ms -> 單次 GET」證明它必定錯過。

  inline C# 相容 Windows PowerShell 5.1 的 C# 5 編譯器（不用 out var / 字串插值 /
  nameof / tuple），PowerShell 7 也能跑。唯讀為主；唯一的寫入是 feature 查詢
  （預設 0x83 GET_ATTRIBUTES_VALUES），對控制器無副作用。

  注意：本機 Steam 若在跑，會跟本腳本搶同一個一次性回應暫存器（實測 Steam 開/關對
  report id / 頻率 / 時序無差異，只是多一個競爭者）；量到 stolen 或全 err 31 時先看
  最後一節的 Steam 狀態。控制器睡眠時沒有任何 input report——先按 Steam 鍵喚醒。

.PARAMETER Seconds   讀取直方圖秒數（0 = 跳過讀取；預設 5）
.PARAMETER Type      feature 查詢 type（預設 0x83 = GET_ATTRIBUTES_VALUES）
.PARAMETER Trials    feature 往返量測次數（預設 3）
.PARAMETER MaxMs     每次輪詢上限 ms（預設 200；實測回應落在 13~21 ms）
.PARAMETER NoFeature 跳過 feature 量測（純唯讀）
.PARAMETER LegacyCheck 額外模擬舊 client 時序（SET -> Sleep 20 ms -> 單次 GET）
.PARAMETER Json      把結果另存 JSON 檔（同時仍印 RESULT: 行）

.EXAMPLE
  pwsh Sunshine\src\platform\windows\sc_hid_driver\diag\sc_hid_probe.ps1 -Seconds 8
  powershell -ExecutionPolicy Bypass -File Sunshine\src\platform\windows\sc_hid_driver\diag\sc_hid_probe.ps1 -Seconds 8 -LegacyCheck

.NOTES
  位置：Sunshine\src\platform\windows\sc_hid_driver\diag\（隨 repo 版控）。repo 的 scripts\ 目錄
  整個是本機 gitignore，診斷腳本放那裡不會進版控。
#>
param(
  [int]$Seconds = 5,
  [int]$Type = 0x83,
  [int]$Trials = 3,
  [int]$MaxMs = 200,
  [switch]$NoFeature,
  [switch]$LegacyCheck,
  [string]$Json = ''
)
$ErrorActionPreference = 'Stop'

$src = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public class ScIface {
  public int Index; public string Path; public int Vid; public int Pid; public int Rev; public int Mi = -1; public int Col = -1;
  public int UsagePage; public int Usage; public int InLen; public int OutLen; public int FeatLen;
  public string Product = ""; public string Serial = ""; public string Manufacturer = "";
  public string InputIds = ""; public string OutputIds = ""; public string FeatureIds = "";
  public int OpenRwErr; public int OpenRoErr; public int CapsStatus;
}

public class ScRx {
  public int Index; public string Path; public int InLen; public int Seconds;
  public int Total; public int OpenErr; public int ReadErr; public double Secs;
  public Dictionary<int,int> Count = new Dictionary<int,int>();
  public Dictionary<int,int> Len = new Dictionary<int,int>();
  public Dictionary<int,string> First = new Dictionary<int,string>();
}

public class ScFeat {
  public int Index; public string Path; public int FeatLen; public int Type; public int MaxMs; public int LegacyDelayMs;
  public int OpenErr; public bool SetOk; public int SetErr; public double SetMs;
  public bool Got; public double LatMs; public int Tries; public int Stolen; public string StolenTypes = "";
  public string Resp = ""; public string ErrHist = ""; public double TotalMs;
}

public static class ScHidProbe {
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
  [DllImport("hid.dll", CharSet=CharSet.Unicode)] static extern bool HidD_GetManufacturerString(IntPtr h, StringBuilder s, uint len);
  [DllImport("hid.dll")] static extern bool HidD_SetNumInputBuffers(IntPtr h, uint n);
  [DllImport("hid.dll", SetLastError=true)] static extern bool HidD_SetFeature(IntPtr h, byte[] b, uint len);
  [DllImport("hid.dll", SetLastError=true)] static extern bool HidD_GetFeature(IntPtr h, byte[] b, uint len);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode)] static extern IntPtr SetupDiGetClassDevsW(ref Guid g, IntPtr e, IntPtr p, uint f);
  [DllImport("setupapi.dll")] static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, uint i, ref DID did);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode)] static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr s, ref DID d, IntPtr det, uint sz, ref uint req, IntPtr dd);
  [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool ReadFile(IntPtr h, IntPtr buf, uint n, out uint got, IntPtr ovl);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetOverlappedResult(IntPtr h, IntPtr ovl, out uint got, bool wait);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateEventW(IntPtr sa, bool manual, bool init, string name);
  [DllImport("kernel32.dll")] static extern bool ResetEvent(IntPtr e);
  [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
  [DllImport("kernel32.dll")] static extern bool CancelIo(IntPtr h);
  [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint ms);
  [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint ms);

  const uint DIGCF_PRESENT = 0x2, DIGCF_DEVICEINTERFACE = 0x10;
  const uint GR = 0x80000000, GW = 0x40000000, SHARE_RW = 0x3, OPEN_EXISTING = 3, FILE_FLAG_OVERLAPPED = 0x40000000;
  const int HIDP_STATUS_SUCCESS = 0x110000;
  static readonly IntPtr INVALID = new IntPtr(-1);

  public static string Hex(byte[] b, int n) {
    if (b == null) return "";
    if (n > b.Length) n = b.Length;
    if (n <= 0) return "";
    return BitConverter.ToString(b, 0, n).Replace("-", " ");
  }

  static IntPtr Open(string path, uint access, uint flags) {
    return CreateFileW(path, access, SHARE_RW, IntPtr.Zero, OPEN_EXISTING, flags, IntPtr.Zero);
  }

  // 走一次 SetupDi HID 介面列舉，回傳所有 VID 相符（pid<0 = 不過濾 PID）的介面。
  public static List<ScIface> Enumerate(int vid, int pid) {
    List<ScIface> list = new List<ScIface>();
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

        // 先用 access=0 開（只查屬性，不會被 exclusive open 擋）
        IntPtr h0 = Open(path, 0, 0);
        if (h0 == INVALID) h0 = Open(path, GR, 0);
        if (h0 == INVALID) continue;
        ATTR a = new ATTR(); a.Size = (uint)Marshal.SizeOf(typeof(ATTR));
        bool gA = HidD_GetAttributes(h0, ref a);
        CloseHandle(h0);
        if (!gA || a.VendorID != vid) continue;
        if (pid >= 0 && a.ProductID != pid) continue;

        ScIface f = new ScIface();
        f.Index = list.Count; f.Path = path; f.Vid = a.VendorID; f.Pid = a.ProductID; f.Rev = a.VersionNumber;
        ParseMiCol(path, f);
        IntPtr hrw = Open(path, GR | GW, 0);
        f.OpenRwErr = (hrw == INVALID) ? Marshal.GetLastWin32Error() : 0;
        IntPtr hro = Open(path, GR, 0);
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
          sb = new StringBuilder(256);
          if (HidD_GetManufacturerString(hc, sb, 512)) f.Manufacturer = sb.ToString();
        }
        if (hrw != INVALID) CloseHandle(hrw);
        if (hro != INVALID) CloseHandle(hro);
        list.Add(f);
      }
    } finally { SetupDiDestroyDeviceInfoList(set); }
    return list;
  }

  static void ParseMiCol(string path, ScIface f) {
    string p = path.ToLowerInvariant();
    int i = p.IndexOf("mi_");
    if (i >= 0 && i + 5 <= p.Length) { int v; if (int.TryParse(p.Substring(i + 3, 2), System.Globalization.NumberStyles.HexNumber, null, out v)) f.Mi = v; }
    i = p.IndexOf("col");
    if (i >= 0 && i + 5 <= p.Length) { int v; if (int.TryParse(p.Substring(i + 3, 2), System.Globalization.NumberStyles.HexNumber, null, out v)) f.Col = v; }
  }

  // 依 report type 走 HidP_GetValueCaps + HidP_GetButtonCaps，彙整成 "0x42(53B) 0x44(5B) ..."
  // （byte 數 = 該 report id 的 bit 總和 / 8，不含 report id 本身；與 HIDP_CAPS 的
  // InputReportByteLength 差 1）。用 raw offset 讀 HIDP_VALUE_CAPS / HIDP_BUTTON_CAPS
  // （皆 72 bytes；ReportID 在 offset 2、IsRange 在 12、BitSize 18、ReportCount 20、
  // UsageMin/Max 56/58），避免 struct 佈局跟 SDK 版本綁死。
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
    foreach (int rid in order) {
      if (sb.Length > 0) sb.Append(' ');
      sb.Append(String.Format("0x{0:X2}({1}B)", rid, (bits[rid] + 7) / 8));
    }
    return sb.ToString();
  }

  // ---- N 秒讀取直方圖（每個介面一條 thread，overlapped ReadFile + 逾時） ----
  public static List<ScRx> ReadAll(List<ScIface> ifs, int seconds) {
    List<ScRx> stats = new List<ScRx>();
    List<Thread> threads = new List<Thread>();
    foreach (ScIface f in ifs) {
      ScRx s = new ScRx(); s.Index = f.Index; s.Path = f.Path; s.InLen = f.InLen; s.Seconds = seconds;
      stats.Add(s);
      Thread t = new Thread(new ParameterizedThreadStart(ReadLoop));
      t.IsBackground = true; threads.Add(t); t.Start(s);
    }
    foreach (Thread t in threads) t.Join(seconds * 1000 + 5000);
    return stats;
  }

  static void ReadLoop(object o) {
    ScRx s = (ScRx)o;
    IntPtr h = Open(s.Path, GR | GW, FILE_FLAG_OVERLAPPED);
    if (h == INVALID) h = Open(s.Path, GR, FILE_FLAG_OVERLAPPED);
    if (h == INVALID) { s.OpenErr = Marshal.GetLastWin32Error(); return; }
    HidD_SetNumInputBuffers(h, 64);   // 與 SDL hidapi 相同
    int bufLen = s.InLen > 0 ? s.InLen : 64;
    if (bufLen < 64) bufLen = 64;
    byte[] buf = new byte[bufLen];
    IntPtr pbuf = Marshal.AllocHGlobal(bufLen);
    int ovlSize = IntPtr.Size == 8 ? 32 : 20;
    int evtOff = IntPtr.Size == 8 ? 24 : 16;
    IntPtr ovl = Marshal.AllocHGlobal(ovlSize);
    IntPtr evt = CreateEventW(IntPtr.Zero, true, false, null);
    Stopwatch sw = Stopwatch.StartNew();
    long limit = (long)s.Seconds * 1000;
    try {
      while (sw.ElapsedMilliseconds < limit) {
        for (int i = 0; i < ovlSize; i++) Marshal.WriteByte(ovl, i, 0);
        Marshal.WriteIntPtr(ovl, evtOff, evt);
        ResetEvent(evt);
        uint got = 0;
        bool ok = ReadFile(h, pbuf, (uint)bufLen, out got, ovl);
        if (!ok) {
          int e = Marshal.GetLastWin32Error();
          if (e == 997) { // ERROR_IO_PENDING
            long remain = limit - sw.ElapsedMilliseconds; if (remain < 1) remain = 1;
            uint w = WaitForSingleObject(evt, (uint)remain);
            if (w != 0) { CancelIo(h); WaitForSingleObject(evt, 200); break; }
            if (!GetOverlappedResult(h, ovl, out got, false)) { s.ReadErr = Marshal.GetLastWin32Error(); break; }
          } else { s.ReadErr = e; break; }
        }
        if (got == 0) continue;
        Marshal.Copy(pbuf, buf, 0, (int)got);
        int id = buf[0];
        s.Total++;
        if (!s.Count.ContainsKey(id)) { s.Count[id] = 0; s.Len[id] = (int)got; s.First[id] = Hex(buf, (int)got); }
        s.Count[id]++;
      }
    } finally {
      s.Secs = sw.Elapsed.TotalSeconds;
      CloseHandle(h); CloseHandle(evt);
      Marshal.FreeHGlobal(pbuf); Marshal.FreeHGlobal(ovl);
    }
  }

  // ---- feature 往返：SET 0x01 [type 00] -> 每 ~1 ms GET 0x01，直到 resp[1]==type 或逾時 ----
  // legacyDelayMs > 0 時改成舊 client 的做法：SET -> Sleep(legacyDelayMs) -> 單次 GET。
  public static ScFeat FeatureRace(ScIface f, int type, int maxMs, int legacyDelayMs) {
    ScFeat r = new ScFeat(); r.Index = f.Index; r.Path = f.Path; r.FeatLen = f.FeatLen; r.Type = type; r.MaxMs = maxMs; r.LegacyDelayMs = legacyDelayMs;
    int len = f.FeatLen > 0 ? f.FeatLen : 64;
    IntPtr h = Open(f.Path, GR | GW, 0);
    if (h == INVALID) { r.OpenErr = Marshal.GetLastWin32Error(); return r; }
    timeBeginPeriod(1);
    Stopwatch sw = Stopwatch.StartNew();
    try {
      byte[] q = new byte[len]; q[0] = 0x01; q[1] = (byte)type; q[2] = 0x00;
      r.SetOk = HidD_SetFeature(h, q, (uint)len);
      r.SetErr = r.SetOk ? 0 : Marshal.GetLastWin32Error();
      r.SetMs = sw.Elapsed.TotalMilliseconds;
      if (!r.SetOk) return r;
      Dictionary<int,int> errs = new Dictionary<int,int>();
      List<int> errOrder = new List<int>();
      if (legacyDelayMs > 0) {
        Thread.Sleep(legacyDelayMs);
        byte[] b = new byte[len]; b[0] = 0x01;
        r.Tries = 1;
        if (HidD_GetFeature(h, b, (uint)len)) {
          if (b[1] == type) { r.Got = true; r.LatMs = sw.Elapsed.TotalMilliseconds; r.Resp = Hex(b, 32); }
          else { r.Stolen++; r.StolenTypes = String.Format("0x{0:X2}", b[1]); }
        } else { int e = Marshal.GetLastWin32Error(); errs[e] = 1; errOrder.Add(e); }
      } else {
        while (sw.ElapsedMilliseconds <= maxMs) {
          byte[] b = new byte[len]; b[0] = 0x01;
          r.Tries++;
          if (HidD_GetFeature(h, b, (uint)len)) {
            if (b[1] == type) { r.Got = true; r.LatMs = sw.Elapsed.TotalMilliseconds; r.Resp = Hex(b, 32); break; }
            r.Stolen++;
            if (r.StolenTypes.Length < 60) r.StolenTypes += String.Format("{0}0x{1:X2}@{2:F0}ms", r.StolenTypes.Length > 0 ? "," : "", b[1], sw.Elapsed.TotalMilliseconds);
          } else {
            int e = Marshal.GetLastWin32Error();
            if (!errs.ContainsKey(e)) { errs[e] = 0; errOrder.Add(e); }
            errs[e]++;
          }
          Thread.Sleep(1);
        }
      }
      StringBuilder sb = new StringBuilder();
      foreach (int e in errOrder) { if (sb.Length > 0) sb.Append(','); sb.Append(String.Format("err{0}x{1}", e, errs[e])); }
      r.ErrHist = sb.ToString();
      r.TotalMs = sw.Elapsed.TotalMilliseconds;
    } finally { timeEndPeriod(1); CloseHandle(h); }
    return r;
  }
}
'@
Add-Type -TypeDefinition $src -Language CSharp

function Fmt-Ids([System.Collections.Generic.Dictionary[int,int]]$d) {
  $parts = @()
  foreach ($kv in ($d.GetEnumerator() | Sort-Object -Property Value -Descending)) { $parts += ('0x{0:X2}={1}' -f $kv.Key, $kv.Value) }
  return ($parts -join ' ')
}

$psv = $PSVersionTable.PSVersion.ToString()
Write-Host ("=== SC-HID client probe  {0}  host={1}  PS {2} ===" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $env:COMPUTERNAME, $psv)

# ---- [1] 列舉 ----
$ifs = [ScHidProbe]::Enumerate(0x28DE, -1)
Write-Host ("[1] HID interfaces with VID 0x28DE: {0}" -f $ifs.Count)
$ifJson = @()
foreach ($f in $ifs) {
  $mi = if ($f.Mi -ge 0) { 'MI_{0:X2}' -f $f.Mi } else { 'MI_--' }
  $col = if ($f.Col -ge 0) { 'Col{0:X2}' -f $f.Col } else { '' }
  $rw = if ($f.OpenRwErr -eq 0) { 'OK' } else { "FAIL err=$($f.OpenRwErr)" }
  $ro = if ($f.OpenRoErr -eq 0) { 'OK' } else { "FAIL err=$($f.OpenRoErr)" }
  Write-Host ("  #{0} PID=0x{1:X4} rev=0x{2:X4} {3} {4} UP=0x{5:X4}/U=0x{6:X4} in={7} out={8} feat={9} open(R+W)={10} open(R)={11}" -f `
    $f.Index, $f.Pid, $f.Rev, $mi, $col, $f.UsagePage, $f.Usage, $f.InLen, $f.OutLen, $f.FeatLen, $rw, $ro)
  Write-Host ("       product='{0}' serial='{1}' mfr='{2}'" -f $f.Product, $f.Serial, $f.Manufacturer)
  Write-Host ("       path={0}" -f $f.Path)
  Write-Host ("       INPUT  : {0}" -f $f.InputIds)
  Write-Host ("       OUTPUT : {0}" -f $f.OutputIds)
  Write-Host ("       FEATURE: {0}" -f $f.FeatureIds)
  $ifJson += [ordered]@{ index=$f.Index; pid=('0x{0:X4}' -f $f.Pid); rev=('0x{0:X4}' -f $f.Rev); mi=$f.Mi; col=$f.Col
                         usagePage=('0x{0:X4}' -f $f.UsagePage); usage=('0x{0:X4}' -f $f.Usage)
                         inLen=$f.InLen; outLen=$f.OutLen; featLen=$f.FeatLen; openRwErr=$f.OpenRwErr; openRoErr=$f.OpenRoErr
                         product=$f.Product; serial=$f.Serial; input=$f.InputIds; output=$f.OutputIds; feature=$f.FeatureIds; path=$f.Path }
}
if ($ifs.Count -eq 0) {
  Write-Host '  (no 0x28DE HID interface: plug the controller / Puck in, or check Device Manager)'
  Write-Host ('RESULT:' + ([ordered]@{ ok=$false; reason='no 0x28DE interface'; interfaces=0 } | ConvertTo-Json -Compress))
  exit 1
}

# ---- [2] 讀取直方圖 ----
$rxJson = @()
$active = $null
$activeScore = 0
# feature 量測的 fallback 候選：vendor page FF00 且 Usage 0x0001（controller slot 介面；
# Puck 的 MI_06 dongle 管理介面是 FF00/0002，對 0x01 feature 沒有控制器回應，排除）。
$vendorIfs = @($ifs | Where-Object { $_.UsagePage -eq 0xFF00 -and $_.Usage -eq 0x0001 -and $_.OpenRwErr -eq 0 })
if ($Seconds -gt 0) {
  Write-Host ("[2] Reading input reports for {0} s on all {1} interface(s) in parallel (press Steam button if the controller sleeps)..." -f $Seconds, $ifs.Count)
  $rx = [ScHidProbe]::ReadAll($ifs, $Seconds)
  foreach ($s in $rx) {
    $rate = if ($s.Secs -gt 0) { $s.Total / $s.Secs } else { 0 }
    $errTxt = ''
    if ($s.OpenErr -ne 0) { $errTxt = " open err=$($s.OpenErr)" }
    if ($s.ReadErr -ne 0) { $errTxt += " read err=$($s.ReadErr)" }
    Write-Host ("  #{0} total={1} ({2:F1}/s over {3:F1}s){4}  ids: {5}" -f $s.Index, $s.Total, $rate, $s.Secs, $errTxt, (Fmt-Ids $s.Count))
    $firsts = [ordered]@{}
    foreach ($kv in ($s.First.GetEnumerator() | Sort-Object -Property Key)) {
      Write-Host ("       first 0x{0:X2} ({1}B): {2}" -f $kv.Key, $s.Len[$kv.Key], $kv.Value)
      $firsts[('0x{0:X2}' -f $kv.Key)] = $kv.Value
    }
    $counts = [ordered]@{}
    foreach ($kv in ($s.Count.GetEnumerator() | Sort-Object -Property Key)) { $counts[('0x{0:X2}' -f $kv.Key)] = $kv.Value }
    $rxJson += [ordered]@{ index=$s.Index; total=$s.Total; secs=[math]::Round($s.Secs,2); ratePerSec=[math]::Round($rate,1); openErr=$s.OpenErr; readErr=$s.ReadErr; counts=$counts; first=$firsts }
    # 活躍 slot = 吐最多 gamepad state（0x42 / 0x45 / 0x47）的介面
    $score = 0
    foreach ($id in @(0x42, 0x45, 0x47)) { if ($s.Count.ContainsKey($id)) { $score += $s.Count[$id] } }
    if ($score -gt $activeScore -and $ifs[$s.Index].OpenRwErr -eq 0) { $activeScore = $score; $active = $ifs[$s.Index] }
  }
  if ($active) {
    Write-Host ("  active slot: #{0} (state reports={1}; 0x42=Puck/USB controller state, 0x45=BLE-style state)" -f $active.Index, $activeScore)
  } else {
    Write-Host '  active slot: none - no 0x42/0x45/0x47 seen. Controller asleep (press the Steam button) or not paired to this Puck.'
  }
} else {
  Write-Host '[2] read skipped (-Seconds 0)'
}
if ((-not $active -or $active.OpenRwErr -ne 0) -and $vendorIfs.Count -gt 0) {
  $active = $vendorIfs[0]
  Write-Host ("  (no state report seen; falling back to first FF00/0001 slot interface #{0} - may not be the live slot)" -f $active.Index)
}

# ---- [3] feature 往返 ----
$featJson = @()
if ($NoFeature) {
  Write-Host '[3] feature round-trip skipped (-NoFeature)'
} elseif (-not $active) {
  Write-Host '[3] feature round-trip skipped: no vendor FF00/Usage 0x0001 interface openable R+W'
} else {
  Write-Host ("[3] Feature round-trip on #{0}: SET 0x01 [type 0x{1:X2}] -> poll GET 0x01 every ~1 ms (max {2} ms), accept only resp[1]==type; x{3}" -f $active.Index, $Type, $MaxMs, $Trials)
  for ($t = 1; $t -le $Trials; $t++) {
    $r = [ScHidProbe]::FeatureRace($active, $Type, $MaxMs, 0)
    if ($r.OpenErr -ne 0) { Write-Host ("  trial {0}: open failed err={1}" -f $t, $r.OpenErr) }
    elseif (-not $r.SetOk) { Write-Host ("  trial {0}: SET failed err={1}" -f $t, $r.SetErr) }
    elseif ($r.Got) { Write-Host ("  trial {0}: SET ok ({1:F1} ms) -> GOT type 0x{2:X2} at {3:F1} ms after {4} GET(s) [{5}] stolen={6}{7}  resp: {8}" -f $t, $r.SetMs, $Type, $r.LatMs, $r.Tries, $r.ErrHist, $r.Stolen, $(if ($r.StolenTypes) { " ($($r.StolenTypes))" } else { '' }), $r.Resp) }
    else { Write-Host ("  trial {0}: SET ok ({1:F1} ms) -> NO matching response within {2:F0} ms after {3} GET(s) [{4}] stolen={5}{6}" -f $t, $r.SetMs, $r.TotalMs, $r.Tries, $r.ErrHist, $r.Stolen, $(if ($r.StolenTypes) { " ($($r.StolenTypes))" } else { '' })) }
    $featJson += [ordered]@{ trial=$t; mode='poll'; openErr=$r.OpenErr; setOk=$r.SetOk; setErr=$r.SetErr; setMs=[math]::Round($r.SetMs,2); got=$r.Got; latMs=[math]::Round($r.LatMs,2); tries=$r.Tries; errHist=$r.ErrHist; stolen=$r.Stolen; stolenTypes=$r.StolenTypes; resp=$r.Resp }
    Start-Sleep -Milliseconds 250
  }
  if ($LegacyCheck) {
    $r = [ScHidProbe]::FeatureRace($active, $Type, $MaxMs, 20)
    $txt = if ($r.Got) { ("GOT at {0:F1} ms (lucky - window is 13~21 ms)" -f $r.LatMs) } elseif ($r.Stolen -gt 0) { "got OTHER type $($r.StolenTypes)" } else { "FAIL [$($r.ErrHist)]" }
    Write-Host ("  legacy (SET -> Sleep 20 ms -> single GET, what the pre-Round-1 client did): {0}" -f $txt)
    $featJson += [ordered]@{ trial=0; mode='legacy20ms'; setOk=$r.SetOk; setErr=$r.SetErr; got=$r.Got; latMs=[math]::Round($r.LatMs,2); errHist=$r.ErrHist; stolen=$r.Stolen; stolenTypes=$r.StolenTypes; resp=$r.Resp }
  }
}

# ---- [4] 本機 Steam ----
$steam = Get-Process steam -ErrorAction SilentlyContinue
$steamTxt = if ($steam) { 'running (PID ' + (($steam | ForEach-Object { $_.Id }) -join ',') + ') - competes for the same one-shot feature response register' } else { 'not running' }
Write-Host ("[4] Steam on this machine: {0}" -f $steamTxt)

$gotCount = @($featJson | Where-Object { $_.mode -eq 'poll' -and $_.got }).Count
$result = [ordered]@{
  ok = ($ifs.Count -gt 0)
  interfaces = $ifs.Count
  activeIndex = $(if ($active) { $active.Index } else { -1 })
  activePid = $(if ($active) { ('0x{0:X4}' -f $active.Pid) } else { '' })
  stateReports = $activeScore
  featureTrials = $Trials
  featureGot = $gotCount
  steamRunning = [bool]$steam
  ifaces = $ifJson
  rx = $rxJson
  feature = $featJson
}
# 注意：PowerShell 變數名不分大小寫，$json 會跟參數 $Json 撞名，所以用 $jsonText。
$jsonText = $result | ConvertTo-Json -Depth 8 -Compress
if ($Json) { $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Json -Encoding UTF8; Write-Host "  json -> $Json" }
Write-Host ('RESULT:' + $jsonText)
