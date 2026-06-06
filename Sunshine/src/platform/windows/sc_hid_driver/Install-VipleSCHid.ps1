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
#                 via VipleSCHid.cpp::tryInstallDriver(). Skips if a healthy node
#                 already exists.
#   -Reinstall    remove existing Viple\SteamController node(s) first, then do a
#                 clean create+bind. Used by the dev reinstall loop / host worker.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File Install-VipleSCHid.ps1
#   powershell -ExecutionPolicy Bypass -File Install-VipleSCHid.ps1 -Reinstall

param(
    [string]$DriverDir = $PSScriptRoot,
    [switch]$Reinstall
)

$ErrorActionPreference = "Stop"
$inf  = Join-Path $DriverDir "VipleSCHid.inf"
$cer  = Join-Path $DriverDir "VipleSCHid_SelfSign.cer"
$hwid = "Viple\SteamController"
$desc = "VipleStream Steam Controller (Virtual)"

function Write-Result($obj) {
    Write-Host ("RESULT:" + ($obj | ConvertTo-Json -Compress))
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
            if (!SetupDiCreateDeviceInfoW(di, "HIDClass", ref HIDClass, null, IntPtr.Zero, DICD_GENERATE_ID, ref dd))
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
if (Test-Path $cer) {
    try {
        Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
        Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
        Write-Host "[SC-HID] self-signed cert imported (Root + TrustedPublisher)."
    } catch {
        Write-Host "[SC-HID] cert import skipped/failed: $($_.Exception.Message)"
    }
}

$present = [VipleDevNode]::CountNodes($hwid)

if (-not $Reinstall -and $present -gt 0) {
    # Gentle mode: a node already exists. Only skip if it is actually healthy;
    # a FAILED_START node should still be repaired by the caller via -Reinstall.
    $dev = Get-PnpDevice -Class HIDClass -ErrorAction SilentlyContinue |
           Where-Object { $_.FriendlyName -like "*Steam Controller*" } | Select-Object -First 1
    if ($dev -and $dev.Status -eq "OK") {
        Write-Result @{ ok = $true; op = "install"; skipped = $true
                        note = "healthy Viple\SteamController node already present"
                        status = "$($dev.Status)"; instanceId = "$($dev.InstanceId)" }
        exit 0
    }
}

# ---- Reinstall: tear down existing nodes for a clean bind ----------------
if ($Reinstall -and $present -gt 0) {
    $removed = [VipleDevNode]::RemoveNodes($hwid)
    Write-Host "[SC-HID] removed $removed existing Viple\SteamController node(s)."
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
$dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
    $h = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName "DEVPKEY_Device_HardwareIds" -ErrorAction SilentlyContinue).Data
    $h -contains $hwid
} | Select-Object -First 1

if (-not $dev) {
    Write-Result @{ ok = $false; op = "verify"; reason = "no Viple\SteamController devnode after install"
                    nodeCount = [VipleDevNode]::CountNodes($hwid) }
    exit 1
}

$svc    = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName "DEVPKEY_Device_Service"      -ErrorAction SilentlyContinue).Data
$lower  = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName "DEVPKEY_Device_LowerFilters" -ErrorAction SilentlyContinue).Data
$upper  = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName "DEVPKEY_Device_UpperFilters" -ErrorAction SilentlyContinue).Data
$prob   = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName "DEVPKEY_Device_ProblemCode"  -ErrorAction SilentlyContinue).Data

# A bare (NULL-driver) root node still reports Status=OK / problemCode=0 because it
# "has no problem" -- it just has no function driver bound. Real success therefore
# requires the function driver to actually be MsHidUmdf, not merely Status=OK.
$bound = ($dev.Status -eq "OK") -and ("$svc" -ieq "MsHidUmdf")

Write-Result @{
    ok           = $bound
    op           = ($(if ($Reinstall) { "reinstall" } else { "install" }))
    status       = "$($dev.Status)"
    problemCode  = "$prob"
    instanceId   = "$($dev.InstanceId)"
    friendlyName = "$($dev.FriendlyName)"
    service      = "$svc"
    lowerFilters = "$($lower -join ',')"
    upperFilters = "$($upper -join ',')"
    bound        = $bound
    expected     = "service=MsHidUmdf, lowerFilters=WUDFRd, status=OK, problemCode=0"
}
exit $(if ($bound) { 0 } else { 2 })
