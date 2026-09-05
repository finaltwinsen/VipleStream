# §SC-HID: Install VipleStream Steam Controller Virtual HID Driver
#
# Requires elevation (admin). Two responsibilities:
#   1. Stage the UMDF2 driver package into the driver store (pnputil /add-driver).
#   2. Create a ROOT-enumerated devnode with HWID "Viple\SteamController" so the
#      driver actually binds and starts. Without (2) the package just sits in the
#      store with nothing to attach to (the symptom we hit: "driver staged but no
#      devnode bound").
#
# The devnode is created via SetupAPI P/Invoke (DIF_REGISTERDEVICE +
# UpdateDriverForPlugAndPlayDevices) — exactly what `devcon install <inf> <hwid>`
# does internally — so we do NOT depend on devcon.exe being present on the host
# (it is a WDK tool and is absent on a plain streaming host; the old devcon-based
# path silently fell back to `pnputil /scan-devices`, which never creates a node).
#
# Modes:
#   (no switch)   install-if-absent — gentle; called by Sunshine at first startup
#                 via VipleSCHid.cpp::tryInstallDriver() and by the host deploy
#                 script after every server update. Skips ONLY if a healthy node
#                 exists AND its bound driver version equals this package's INF
#                 DriverVer; a healthy node on an older DriverVer escalates to
#                 -Reinstall automatically (RESULT autoReinstall=true). Rationale
#                 (§SC-HID Round 1, 2026-09-02): pnputil /add-driver silently keeps
#                 the old package when DriverVer is unchanged and PnP never rebinds
#                 a happy node, so a freshly copied DLL never took effect on host.
#   -Reinstall    remove existing Viple\VID_28DE&PID_1302 node(s) + old driver
#                 packages first, then do a clean create+bind. Refused (exit 3)
#                 unless the signing cert's thumbprint is verified to be present in
#                 Cert:\LocalMachine\TrustedPublisher (+ Root) AT THAT MOMENT —
#                 either the shipped .cer just imported + re-read from the store,
#                 or the DLL's Authenticode signer already trusted. A .cer merely
#                 existing on disk is NOT enough (import can fail silently in a
#                 service session) — deleting the old package would leave nothing
#                 installable.
#   -Diag         read-only: replicate the server's openHidByVidPid and report.
#
# RESULT line (last stdout line, "RESULT:{json}"): ok / op / bound / status /
#   service / driverVersion (bound node's DEVPKEY_Device_DriverVersion) /
#   infVersion (this package's INF DriverVer) / versionMatch / autoReinstall.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File Install-VipleSCHid.ps1
#   powershell -ExecutionPolicy Bypass -File Install-VipleSCHid.ps1 -Reinstall

param(
    [string]$DriverDir = $PSScriptRoot,
    [switch]$Reinstall,
    [switch]$Diag        # diagnose why the server's openHidByVidPid can't open the virtual device
)

$ErrorActionPreference = "Stop"
# Cert: provider + Get-AuthenticodeSignature 都在 Microsoft.PowerShell.Security 模組。
# Windows PowerShell 5.1 若繼承了 PowerShell 7 的 PSModulePath（由 pwsh 型 host worker
# 或 pwsh 殼啟動 powershell.exe 時會發生），會誤載 7 版同名模組 → TypeData 衝突載入
# 失敗 → Cert: 提供者不存在（實測：CouldNotAutoloadMatchingModule）。載不到時
# Test-CertInStore 一律回 false → -Reinstall 被拒（安全方向：寧可不刪舊套件），
# 所以這裡先明確載入；失敗就把 PSModulePath 重設成 5.1 預設路徑再試一次。
# （那個載入失敗是終止型 FormatXmlUpdateException，-ErrorAction 壓不住，必須 try/catch。）
try { Import-Module Microsoft.PowerShell.Security -ErrorAction Stop } catch { }
if (-not (Get-PSDrive Cert -ErrorAction SilentlyContinue) -and $PSVersionTable.PSVersion.Major -le 5) {
    $env:PSModulePath = "$env:ProgramFiles\WindowsPowerShell\Modules;$env:SystemRoot\system32\WindowsPowerShell\v1.0\Modules"
    try { Import-Module Microsoft.PowerShell.Security -ErrorAction Stop } catch { }
    if (Get-PSDrive Cert -ErrorAction SilentlyContinue) {
        Write-Host "[SC-HID] note: Security module loaded only after resetting PSModulePath (inherited PowerShell 7 path)."
    } else {
        Write-Host "[SC-HID] WARN: Cert: provider unavailable - cert trust checks will fail closed (-Reinstall refused)."
    }
}
$inf  = Join-Path $DriverDir "VipleSCHid.inf"
$cer  = Join-Path $DriverDir "VipleSCHid_SelfSign.cer"
$dll  = Join-Path $DriverDir "VipleSCHid_Driver.dll"
# SC-HID identity fix 2026-07-02：HWID 必須內嵌 VID/PID。HIDClass 對
# root-enumerated 裝置的子 HID 節點 hardware id = 父 HWID 去掉 enumerator
# 前綴、加上 "HID\" —— 舊 HWID "Viple\SteamController" 讓子節點變成
# "HID\SteamController"（無 VID/PID、interface path 無 vid_28de），Steam
# 的控制器枚舉因此不認得它。改成 Viple\VID_28DE&PID_1302 之後子節點是
# "HID\VID_28DE&PID_1302"、path 是 hid#vid_28de&pid_1302#...，與實體
# gen-2 SC（USB 直連）一致。
$hwid = "Viple\VID_28DE&PID_1302"
$legacyHwid = "Viple\SteamController"   # 2026-07-02 之前的舊 HWID，見上
$desc = "VipleStream Steam Controller (Virtual)"

# 憑證匯入狀態（只有下方匯入區塊「匯入成功且回讀店內確認 thumbprint 存在」才會設
# 為 $true；Test-SignerTrusted 以它為第一優先依據）。
$script:certImported = $false
$script:certThumb    = ""

function Write-Result($obj) {
    Write-Host ("RESULT:" + ($obj | ConvertTo-Json -Compress))
}

# INF 的 DriverVer 版本段："07/02/2026,1.0.4.0" -> "1.0.4.0"。解析不到回空字串
#（呼叫端把空字串當「無法比對」處理，不會因此觸發自動重裝）。
function Get-InfDriverVer([string]$Path) {
    try {
        $txt = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
        $m = [regex]::Match($txt, '(?im)^\s*DriverVer\s*=\s*[^,\r\n]*,\s*([0-9]+(?:\.[0-9]+){1,3})')
        if ($m.Success) { return $m.Groups[1].Value }
    } catch { }
    return ""
}

# 找出帶我們 HWID 的 root devnode。用 HARDWARE ID 比對而不是 -Class HIDClass /
# FriendlyName：NULL-driver 或未啟動的節點沒有 HIDClass 關聯，用 class 會漏掉失敗
# 案例；FriendlyName "*Steam Controller*" 又可能抓到子 HID 節點（它綁的是 inbox
# HID driver，DriverVersion 是 Windows 版號，拿來比 INF DriverVer 會永遠不符）。
# 只掃 ROOT\* 節點（我們的節點一律 root-enumerated），避免每台裝置都查一次屬性。
function Find-VipleNode([string]$Hwid) {
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like 'ROOT\*' } |
        Where-Object {
            $h = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName "DEVPKEY_Device_HardwareIds" -ErrorAction SilentlyContinue).Data
            $h -contains $Hwid
        } | Select-Object -First 1
}

function Get-NodeProp([string]$InstanceId, [string]$Key) {
    $d = (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $Key -ErrorAction SilentlyContinue).Data
    if ($null -eq $d) { return "" }
    return "$($d -join ',')"
}

# 此刻店內是否真的有這張憑證（以 thumbprint 比對；$Store = Root / TrustedPublisher）。
function Test-CertInStore([string]$Thumb, [string]$Store) {
    if (-not $Thumb) { return $false }
    $hit = @(Get-ChildItem -Path "Cert:\LocalMachine\$Store" -ErrorAction SilentlyContinue |
             Where-Object { $_.Thumbprint -eq $Thumb })
    return ($hit.Count -gt 0)
}

# 讀 .cer 的 thumbprint（讀不到回空字串）。
function Get-CerThumbprint([string]$CerPath) {
    try {
        $c = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($CerPath)
        return "$($c.Thumbprint)"
    } catch { return "" }
}

# -Reinstall 前置檢查：刪掉舊套件之後，若新套件的簽章憑證不被信任，pnputil
# /add-driver 會拒絕（或在非互動 session 卡住），主機就停在「沒有任何 driver」。
# 所以刪之前必須確認「簽章憑證的 thumbprint 此刻確實在 Cert:\LocalMachine\
# TrustedPublisher（發行者信任）與 Root（自簽 = 鏈的根）」，依序看：
#   1. 本次匯入區塊成功（$script:certImported，匯入後已回讀店內驗過 thumbprint）；
#   2. .cer 隨包但匯入沒成功／被跳過：只有它的 thumbprint 此刻真的在店內才放行；
#   3. 簽 DLL 的憑證：Get-AuthenticodeSignature = Valid（鏈可信）且 thumbprint 在
#      TrustedPublisher。
# 「.cer 檔存在」本身不放行——匯入在服務 session 可能靜默失敗（F8 之前 .cer 甚至
# 不在 zip 內），存在 ≠ 已信任。
function Test-SignerTrusted([string]$DllPath) {
    # 唯一權威 = 即將安裝的 DLL 的 Authenticode 簽章者：鏈 Valid 且 thumbprint 此刻在
    # TrustedPublisher。隨包 .cer / 本次匯入的憑證只是「匯入動作」的紀錄，若它們與
    # DLL 簽章者不是同一張，一律不放行（一張過期或不相干但已被信任的 .cer 不能替
    # NotSigned / 換了簽章的 DLL 背書）。
    $why = @()
    if (-not (Test-Path $DllPath)) {
        return @{ ok = $false; reason = "driver DLL missing: $DllPath -> deleting the old package would leave no installable driver" }
    }
    $sig = $null
    try { $sig = Get-AuthenticodeSignature -FilePath $DllPath -ErrorAction Stop } catch { }
    if (-not $sig -or -not $sig.SignerCertificate) {
        return @{ ok = $false; reason = "driver DLL is NOT signed ($DllPath) -> deleting the old package would leave no installable driver; run Build-ScHidDriver.ps1 in an admin shell first" }
    }
    $thumb = "$($sig.SignerCertificate.Thumbprint)"
    $inTP  = Test-CertInStore $thumb 'TrustedPublisher'
    $inRoot = Test-CertInStore $thumb 'Root'
    if (Test-Path $cer) {
        $t = Get-CerThumbprint $cer
        if ($t -and ($t -ne $thumb)) { $why += "shipped .cer thumb=$t does NOT match DLL signer $thumb" }
        if (-not $t) { $why += "shipped .cer unreadable ($cer)" }
    }
    if ($script:certImported -and $script:certThumb -and ($script:certThumb -ne $thumb)) {
        $why += "cert imported this run ($($script:certThumb)) is not the DLL signer $thumb"
    }
    if ($sig.Status -eq 'Valid' -and $inTP -and $why.Count -eq 0) {
        return @{ ok = $true; reason = "DLL signer $thumb chain Valid + present in TrustedPublisher (Root=$inRoot)" }
    }
    $why += "DLL signer $thumb sigStatus=$($sig.Status) trustedPublisher=$inTP root=$inRoot"
    return @{ ok = $false; reason = (($why -join '; ') + " -> deleting the old package would leave no installable driver") }
}

# ---- -Diag: replicate Sunshine's openHidByVidPid(0x28DE,0x1302) to find out
#      WHY the server can't open the virtual device to inject reports. -------
if ($Diag) {
    $diagCs = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class VHidDiag {
  [StructLayout(LayoutKind.Sequential)] struct DID { public uint cbSize; public Guid g; public uint Flags; public IntPtr Reserved; }
  [StructLayout(LayoutKind.Sequential)] public struct ATTR { public uint Size; public ushort VendorID; public ushort ProductID; public ushort VersionNumber; }
  [StructLayout(LayoutKind.Sequential)] public struct CAPS { public ushort Usage; public ushort UsagePage; public ushort InLen; public ushort OutLen; public ushort FeatLen;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst=17)] public ushort[] R; public ushort a,b,c,d,e,f,g,h,i,j,k; }
  [DllImport("hid.dll")] static extern void HidD_GetHidGuid(out Guid g);
  [DllImport("hid.dll")] static extern bool HidD_GetAttributes(IntPtr h, ref ATTR a);
  [DllImport("hid.dll")] static extern bool HidD_GetPreparsedData(IntPtr h, out IntPtr pp);
  [DllImport("hid.dll")] static extern bool HidD_FreePreparsedData(IntPtr pp);
  [DllImport("hid.dll")] static extern int HidP_GetCaps(IntPtr pp, ref CAPS caps);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode)] static extern IntPtr SetupDiGetClassDevsW(ref Guid g, IntPtr e, IntPtr p, uint f);
  [DllImport("setupapi.dll")] static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, uint i, ref DID d2);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode)] static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr s, ref DID d, IntPtr det, uint sz, ref uint req, IntPtr dd);
  [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  const uint PRES=0x2, DIF=0x10, GR=0x80000000, GW=0x40000000, SH=0x3, OE=3, OVL=0x40000000;
  static IntPtr Op(string p, uint a){ return CreateFileW(p, a, SH, IntPtr.Zero, OE, OVL, IntPtr.Zero); }
  static string En(int e){ if(e==5)return "ACCESS_DENIED"; if(e==32)return "SHARING_VIOLATION"; if(e==2)return "FILE_NOT_FOUND"; if(e==0)return ""; return ""; }
  public static string Go() {
    var sb=new StringBuilder(); Guid hg; HidD_GetHidGuid(out hg);
    IntPtr set=SetupDiGetClassDevsW(ref hg, IntPtr.Zero, IntPtr.Zero, PRES|DIF);
    var did=new DID(); did.cbSize=(uint)Marshal.SizeOf(did); int n=0;
    for(uint i=0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref hg, i, ref did); i++){
      uint req=0; SetupDiGetDeviceInterfaceDetailW(set, ref did, IntPtr.Zero, 0, ref req, IntPtr.Zero);
      did.cbSize=(uint)Marshal.SizeOf(did); if(req==0) continue;
      IntPtr det=Marshal.AllocHGlobal((int)req); Marshal.WriteInt32(det, IntPtr.Size==8?8:6);
      string path=null;
      if(SetupDiGetDeviceInterfaceDetailW(set, ref did, det, req, ref req, IntPtr.Zero)) path=Marshal.PtrToStringUni((IntPtr)((long)det+4));
      Marshal.FreeHGlobal(det); if(path==null) continue;
      IntPtr h0=Op(path,0); if(h0==(IntPtr)(-1)) h0=Op(path,GR); if(h0==(IntPtr)(-1)) continue;
      var a=new ATTR(); a.Size=(uint)Marshal.SizeOf(a); bool gA=HidD_GetAttributes(h0, ref a); CloseHandle(h0);
      if(!(gA && a.VendorID==0x28DE)) continue; n++;
      var caps=new CAPS(); IntPtr pp; string cs="";
      IntPtr hc=Op(path,GR); if(hc!=(IntPtr)(-1)){ if(HidD_GetPreparsedData(hc, out pp)){ HidP_GetCaps(pp, ref caps); HidD_FreePreparsedData(pp);} CloseHandle(hc); cs=String.Format("UP=0x{0:X4}/U=0x{1:X2} In={2}", caps.UsagePage, caps.Usage, caps.InLen); }
      IntPtr hrw=Op(path,GR|GW); int erw=(hrw==(IntPtr)(-1))?Marshal.GetLastWin32Error():0; if(hrw!=(IntPtr)(-1)) CloseHandle(hrw);
      IntPtr hr =Op(path,GR);    int er =(hr ==(IntPtr)(-1))?Marshal.GetLastWin32Error():0; if(hr !=(IntPtr)(-1)) CloseHandle(hr);
      sb.AppendLine(String.Format("PID=0x{0:X4} {1}", a.ProductID, cs));
      sb.AppendLine("    open R+W (== server openHidByVidPid): "+(erw==0?"OK":("FAIL err="+erw+" "+En(erw))));
      sb.AppendLine("    open R-only                        : "+(er ==0?"OK":("FAIL err="+er +" "+En(er ))));
    }
    SetupDiDestroyDeviceInfoList(set);
    if(n==0) sb.AppendLine("(no 0x28DE HID interface enumerated -> server would also see 'not found')");
    return sb.ToString();
  }
}
'@
    Add-Type -TypeDefinition $diagCs -Language CSharp
    Write-Host "=== SC-HID server-open diagnostic (replicates openHidByVidPid) ==="
    Write-Host ([VHidDiag]::Go())
    $steam = Get-Process steam -ErrorAction SilentlyContinue
    Write-Host ("Steam on host: " + $(if ($steam) { "running (PID " + ($steam.Id -join ',') + ")" } else { "NOT running" }))
    Write-Host "(server runs as the VipleStreamServer service. If R+W fails but R-only OK -> write/sharing; if both fail -> access/session; if no device -> enumeration.)"
    exit 0
}

if (-not (Test-Path $inf)) {
    Write-Result @{ ok = $false; reason = "VipleSCHid.inf not found at $inf" }
    exit 1
}

# ---- SetupAPI devnode helper (devcon-free) -------------------------------
# Mirrors `devcon install`: register a root-enumerated devnode for $hwid, then
# install the matching driver onto it. Returns 0 on success, else a tagged
# Win32 error (high nibble marks which step failed) so callers can diagnose.
$cs = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;

public static class VipleDevNode {
    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA {
        public uint cbSize;
        public Guid ClassGuid;
        public uint DevInst;
        public IntPtr Reserved;
    }

    const uint DIGCF_PRESENT      = 0x00000002;
    const uint DIGCF_ALLCLASSES   = 0x00000004;
    const uint DICD_GENERATE_ID   = 0x00000001;
    const uint SPDRP_HARDWAREID   = 0x00000001;
    const uint DIF_REGISTERDEVICE = 0x00000019;
    const uint DIF_REMOVE         = 0x00000005;
    const uint INSTALLFLAG_FORCE  = 0x00000001;

    static Guid HIDClass = new Guid("745a17a0-74d3-11d0-b6fe-00a0c90f57da");

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid ClassGuid, IntPtr hwndParent);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiGetClassDevsW(ref Guid ClassGuid, IntPtr Enumerator, IntPtr hwndParent, uint Flags);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiCreateDeviceInfoW(IntPtr DeviceInfoSet, string DeviceName,
        ref Guid ClassGuid, string DeviceDescription, IntPtr hwndParent, uint CreationFlags,
        ref SP_DEVINFO_DATA DeviceInfoData);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiEnumDeviceInfo(IntPtr DeviceInfoSet, uint MemberIndex, ref SP_DEVINFO_DATA DeviceInfoData);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData,
        uint Property, out uint PropertyRegDataType, byte[] PropertyBuffer, uint PropertyBufferSize, out uint RequiredSize);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr DeviceInfoSet,
        ref SP_DEVINFO_DATA DeviceInfoData, uint Property, byte[] PropertyBuffer, uint PropertyBufferSize);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiCallClassInstaller(uint InstallFunction, IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);
    [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwndParent, string HardwareId,
        string FullInfPath, uint InstallFlags, out bool bRebootRequired);

    static bool HwidMatches(IntPtr set, ref SP_DEVINFO_DATA dd, string hwid) {
        uint regType, req;
        byte[] buf = new byte[2048];
        if (!SetupDiGetDeviceRegistryPropertyW(set, ref dd, SPDRP_HARDWAREID, out regType, buf, (uint)buf.Length, out req))
            return false;
        string s = Encoding.Unicode.GetString(buf, 0, (int)req);
        foreach (string id in s.Split('\0'))
            if (id.Equals(hwid, StringComparison.OrdinalIgnoreCase)) return true;
        return false;
    }

    // Count present devnodes carrying the given hardware id.
    public static int CountNodes(string hwid) {
        // ALLCLASSES so we still find a node that lost its HIDClass association after
        // a failed/NULL-driver install (the broken ROOT\HIDCLASS node we must clean).
        IntPtr set = SetupDiGetClassDevsW(ref HIDClass, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_ALLCLASSES);
        if (set == new IntPtr(-1)) return -1;
        int n = 0;
        try {
            SP_DEVINFO_DATA dd = new SP_DEVINFO_DATA();
            dd.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            for (uint i = 0; SetupDiEnumDeviceInfo(set, i, ref dd); i++) {
                if (HwidMatches(set, ref dd, hwid)) n++;
                dd.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            }
        } finally { SetupDiDestroyDeviceInfoList(set); }
        return n;
    }

    // Remove every present devnode carrying the given hardware id. Returns count removed.
    public static int RemoveNodes(string hwid) {
        // ALLCLASSES so we still find a node that lost its HIDClass association after
        // a failed/NULL-driver install (the broken ROOT\HIDCLASS node we must clean).
        IntPtr set = SetupDiGetClassDevsW(ref HIDClass, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_ALLCLASSES);
        if (set == new IntPtr(-1)) return -1;
        int removed = 0;
        try {
            SP_DEVINFO_DATA dd = new SP_DEVINFO_DATA();
            dd.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            for (uint i = 0; SetupDiEnumDeviceInfo(set, i, ref dd); i++) {
                if (HwidMatches(set, ref dd, hwid)) {
                    if (SetupDiCallClassInstaller(DIF_REMOVE, set, ref dd)) removed++;
                }
                dd.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            }
        } finally { SetupDiDestroyDeviceInfoList(set); }
        return removed;
    }

    // Register a fresh root-enumerated devnode for $hwid. Returns 0 or tagged Win32 error.
    public static uint CreateNode(string hwid) {
        IntPtr di = SetupDiCreateDeviceInfoList(ref HIDClass, IntPtr.Zero);
        if (di == new IntPtr(-1)) return ((uint)Marshal.GetLastWin32Error()) | 0x10000000u;
        try {
            SP_DEVINFO_DATA dd = new SP_DEVINFO_DATA();
            dd.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            // SC-HID identity fix 2026-07-02: the device-name segment here becomes
            // the ROOT instance id (ROOT\VID_28DE&PID_1302\000x). HIDClass reuses
            // the parent's device-name segment for the child HID PDO instance id,
            // which in turn becomes the interface path. With the old "HIDClass"
            // name the child path was \\?\hid#hidclass#... -- no vid/pid in the
            // path, and Steam's controller identification parses the path string.
            // Now the child path is \\?\hid#vid_28de&pid_1302#..., matching a
            // real gen-2 Steam Controller.
            if (!SetupDiCreateDeviceInfoW(di, "VID_28DE&PID_1302", ref HIDClass, null, IntPtr.Zero, DICD_GENERATE_ID, ref dd))
                return ((uint)Marshal.GetLastWin32Error()) | 0x20000000u;
            byte[] buf = Encoding.Unicode.GetBytes(hwid + "\0\0"); // REG_MULTI_SZ
            if (!SetupDiSetDeviceRegistryPropertyW(di, ref dd, SPDRP_HARDWAREID, buf, (uint)buf.Length))
                return ((uint)Marshal.GetLastWin32Error()) | 0x30000000u;
            if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, di, ref dd))
                return ((uint)Marshal.GetLastWin32Error()) | 0x40000000u;
            return 0;
        } finally { SetupDiDestroyDeviceInfoList(di); }
    }

    // Install/bind the matching driver onto present nodes for $hwid.
    public static uint InstallDriver(string hwid, string infPath) {
        bool reboot;
        if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hwid, infPath, INSTALLFLAG_FORCE, out reboot))
            return (uint)Marshal.GetLastWin32Error();
        return 0;
    }
}
'@
Add-Type -TypeDefinition $cs -Language CSharp

# ---- Optional: import the self-signed cert if shipped alongside ----------
# The build signs the DLL/CAT with a self-signed cert. For a manual / host-worker
# install the cert must be trusted (Root + TrustedPublisher) or pnputil rejects
# the package. When bundled inside the signed Sunshine installer this .cer is
# absent and this block is skipped.
# $script:certImported is set ONLY when both imports ran without error AND the
# thumbprint is read back from both stores afterwards (Test-SignerTrusted relies
# on this; a silently failed import must not unlock -Reinstall).
if (Test-Path $cer) {
    try {
        $script:certThumb = Get-CerThumbprint $cer
        if (-not $script:certThumb) { throw "cannot read certificate $cer" }
        Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
        Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
        $inRoot = Test-CertInStore $script:certThumb 'Root'
        $inTP   = Test-CertInStore $script:certThumb 'TrustedPublisher'
        if ($inRoot -and $inTP) {
            $script:certImported = $true
            Write-Host "[SC-HID] self-signed cert imported and verified in store (thumb=$($script:certThumb), Root + TrustedPublisher)."
        } else {
            Write-Host "[SC-HID] cert import ran but thumbprint $($script:certThumb) NOT found afterwards (Root=$inRoot TrustedPublisher=$inTP) - -Reinstall will be refused unless the DLL signer is already trusted."
        }
    } catch {
        Write-Host "[SC-HID] cert import skipped/failed: $($_.Exception.Message)"
    }
}

# ---- 0) Legacy HWID cleanup（無條件）：舊 HWID 的節點身分錯誤（子節點
#      無 VID/PID），留著只會讓 Steam 看到一個「不是 Steam Controller」的
#      裝置，一律移除。----------------------------------------------------
$legacyRemoved = [VipleDevNode]::RemoveNodes($legacyHwid)
if ($legacyRemoved -gt 0) {
    Write-Host "[SC-HID] removed $legacyRemoved legacy $legacyHwid node(s) (pre-identity-fix)."
}

$present = [VipleDevNode]::CountNodes($hwid)
$infVer  = Get-InfDriverVer $inf
if (-not $infVer) { Write-Host "[SC-HID] WARN: could not parse DriverVer from $inf (version compare disabled)." }

# $Reinstall 是 switch 參數；自動升級路徑需要改寫它，所以複製成一般布林變數。
$doReinstall   = [bool]$Reinstall
$autoReinstall = $false

if (-not $doReinstall -and $present -gt 0) {
    # Gentle mode: a node already exists. Skip only if it is healthy (Status OK
    # AND function driver really is MsHidUmdf -- a bare NULL-driver root node also
    # reports Status=OK) AND already bound to this package's INF DriverVer.
    # A healthy node on an older DriverVer means a newer driver was copied next
    # to the exe but never bound: pnputil /add-driver keeps the old package
    # silently when DriverVer is unchanged and PnP never rebinds a happy node
    # (F7 in the §SC-HID Round 1 plan) -> escalate to -Reinstall automatically.
    # A FAILED_START / NULL-driver node falls through to the install path.
    $dev = Find-VipleNode $hwid
    $boundVer = ""; $svcNow = ""
    if ($dev) {
        $boundVer = Get-NodeProp $dev.InstanceId "DEVPKEY_Device_DriverVersion"
        $svcNow   = Get-NodeProp $dev.InstanceId "DEVPKEY_Device_Service"
    }
    $healthy = ($dev -and $dev.Status -eq "OK" -and ("$svcNow" -ieq "MsHidUmdf"))
    $canCompare = ($infVer -ne "") -and ($boundVer -ne "")
    if ($healthy -and ((-not $canCompare) -or ($boundVer -eq $infVer))) {
        Write-Result @{ ok = $true; op = "install"; skipped = $true
                        note = "healthy $hwid node already present at DriverVer '$boundVer'"
                        status = "$($dev.Status)"; instanceId = "$($dev.InstanceId)"
                        service = "$svcNow"; bound = $true
                        driverVersion = "$boundVer"; infVersion = "$infVer"
                        versionMatch = $(if ($canCompare) { $true } else { "unknown" })
                        autoReinstall = $false }
        exit 0
    }
    if ($healthy) {
        Write-Host "[SC-HID] node healthy but bound DriverVer '$boundVer' != INF DriverVer '$infVer' -> auto -Reinstall."
        $doReinstall   = $true
        $autoReinstall = $true
    }
}

# ---- Reinstall: tear down existing nodes + driver packages for a clean bind --
if ($doReinstall) {
    $trust = Test-SignerTrusted $dll
    if (-not $trust.ok) {
        # 舊節點 / 舊套件原封不動留著（沒刪任何東西），讓 host 至少維持今日行為。
        Write-Result @{ ok = $false; op = "reinstall"; refused = $true
                        reason = "refusing -Reinstall: $($trust.reason)"
                        autoReinstall = $autoReinstall; infVersion = "$infVer" }
        exit 3
    }
    Write-Host "[SC-HID] cert precheck OK: $($trust.reason)"
    if ($present -gt 0) {
        $removed = [VipleDevNode]::RemoveNodes($hwid)
        Write-Host "[SC-HID] removed $removed existing $hwid node(s)."
    }
    # Delete any previously-published VipleSCHid driver PACKAGE(s) from the store.
    # Without this, pnputil /add-driver sees the old package as "already exists in
    # the system" and SILENTLY SKIPS importing the new DLL (the bug that left an
    # old DLL bound after a rebuild). Also clears oem* leftovers from prior rounds.
    # Locale-independent: match the value 'vipleschid.inf', not the (localized)
    # field labels.
    $enum = (& pnputil /enum-drivers 2>&1) -join "`n"
    foreach ($blk in ($enum -split "(?m)^\s*$")) {
        if ($blk -match 'vipleschid\.inf' -and $blk -match '(oem\d+\.inf)') {
            Write-Host "[SC-HID] delete old driver package $($matches[1]) ..."
            & pnputil /delete-driver $matches[1] /uninstall /force 2>&1 | Write-Host
        }
    }
    Start-Sleep -Milliseconds 500
}

# ---- 1) Stage the driver package into the store --------------------------
Write-Host "[SC-HID] pnputil /add-driver ..."
& pnputil /add-driver $inf /install | Write-Host
if ($LASTEXITCODE -ne 0) {
    Write-Result @{ ok = $false; op = "add-driver"; reason = "pnputil /add-driver exit $LASTEXITCODE" }
    exit 1
}

# ---- 2) Create the root devnode (if absent) and bind the driver ----------
if ([VipleDevNode]::CountNodes($hwid) -le 0) {
    $rc = [VipleDevNode]::CreateNode($hwid)
    if ($rc -ne 0) {
        Write-Result @{ ok = $false; op = "create-node"; reason = ("CreateNode rc=0x{0:X8}" -f $rc) }
        exit 1
    }
    Write-Host "[SC-HID] root devnode created for $hwid."
}

$rc2 = [VipleDevNode]::InstallDriver($hwid, $inf)
if ($rc2 -ne 0) {
    # Non-fatal: the package is staged and the node exists; PnP may still bind on
    # the next rescan. Surface the code so we can tell ERROR_NO_SUCH_DEVINST etc.
    Write-Host ("[SC-HID] UpdateDriverForPlugAndPlayDevices rc=0x{0:X8} (continuing)" -f $rc2)
}

& pnputil /scan-devices | Out-Null
Start-Sleep -Seconds 3

# ---- 3) Report final device status + driver stack ------------------------
# Find by HARDWARE ID (class-independent): a NULL-driver / not-started node has no
# HIDClass association, so filtering by -Class HIDClass would miss the failure case.
$dev = Find-VipleNode $hwid

if (-not $dev) {
    Write-Result @{ ok = $false; op = "verify"; reason = "no $hwid devnode after install"
                    nodeCount = [VipleDevNode]::CountNodes($hwid)
                    infVersion = "$infVer"; autoReinstall = $autoReinstall }
    exit 1
}

$svc    = Get-NodeProp $dev.InstanceId "DEVPKEY_Device_Service"
$lower  = Get-NodeProp $dev.InstanceId "DEVPKEY_Device_LowerFilters"
$upper  = Get-NodeProp $dev.InstanceId "DEVPKEY_Device_UpperFilters"
$prob   = Get-NodeProp $dev.InstanceId "DEVPKEY_Device_ProblemCode"
$drvVer = Get-NodeProp $dev.InstanceId "DEVPKEY_Device_DriverVersion"
$drvInf = Get-NodeProp $dev.InstanceId "DEVPKEY_Device_DriverInfPath"

# A bare (NULL-driver) root node still reports Status=OK / problemCode=0 because it
# "has no problem" -- it just has no function driver bound. Real success therefore
# requires the function driver to actually be MsHidUmdf, not merely Status=OK.
$bound = ($dev.Status -eq "OK") -and ("$svc" -ieq "MsHidUmdf")
# versionMatch 是「這次真的把新 driver 綁上去了」的證據：bound 但版本仍舊 =
# pnputil 又靜默沿用舊套件（ok 仍照 bound 回報，交由 deploy log 讓人看見）。
$versionMatch = $(if ($infVer -and $drvVer) { ($drvVer -eq $infVer) } else { "unknown" })
if ($bound -and ($versionMatch -eq $false)) {
    Write-Host "[SC-HID] WARN: bound DriverVer '$drvVer' still != INF DriverVer '$infVer' after install."
}

Write-Result @{
    ok            = $bound
    op            = ($(if ($doReinstall) { "reinstall" } else { "install" }))
    autoReinstall = $autoReinstall
    status        = "$($dev.Status)"
    problemCode   = "$prob"
    instanceId    = "$($dev.InstanceId)"
    friendlyName  = "$($dev.FriendlyName)"
    service       = "$svc"
    lowerFilters  = "$lower"
    upperFilters  = "$upper"
    bound         = $bound
    driverVersion = "$drvVer"
    driverInf     = "$drvInf"
    infVersion    = "$infVer"
    versionMatch  = $versionMatch
    expected      = "service=MsHidUmdf, lowerFilters=WUDFRd, status=OK, problemCode=0, driverVersion=$infVer"
}
exit $(if ($bound) { 0 } else { 2 })
