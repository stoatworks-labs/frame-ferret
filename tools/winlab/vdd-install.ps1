# Installs the Virtual Display Driver (VirtualDrivers/Virtual-Display-Driver, MIT,
# signed by the SignPath Foundation) on a TEST machine, with no prompts, so the
# Windows virtual-display node can be run over ssh. This is bench tooling: an
# operator installs VDD with its own installer (see docs/USER-GUIDE.md), and
# Frame Ferret never installs a driver itself. vdd-remove.ps1 undoes every step.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File vdd-install.ps1 -Package <dir>
#
# <dir> is the unpacked VirtualDisplayDriver folder from a VDD release's
# "Driver.Only" zip (MttVDD.inf, MttVDD.dll, mttvdd.cat, vdd_settings.xml).
# Run elevated (an administrator's ssh session is).
#
# Steps:
# 1. The settings file goes to C:\VirtualDisplayDriver, and the registry value
#    the driver reads (HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver\VDDPATH)
#    points there.
# 2. The certificate that signed mttvdd.cat goes into the machine's
#    TrustedPublisher store, so installing the package asks nothing.
# 3. A root-enumerated device with hardware ID Root\MttVDD is created and the
#    driver installed on it: what devcon's `install` does. Windows has no
#    built-in command for it, so it is SetupAPI through Add-Type.
param(
  [Parameter(Mandatory = $true)][string]$Package,
  [string]$SettingsDir = 'C:\VirtualDisplayDriver'
)
$ErrorActionPreference = 'Stop'

$inf = Join-Path $Package 'MttVDD.inf'
$cat = Join-Path $Package 'mttvdd.cat'
foreach ($f in @($inf, $cat, (Join-Path $Package 'MttVDD.dll'), (Join-Path $Package 'vdd_settings.xml'))) {
  if (-not (Test-Path $f)) { throw "missing $f" }
}

$existing = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
  Where-Object { $_.HardwareID -contains 'Root\MttVDD' }
if ($existing) {
  "VDD is already installed: $($existing.InstanceId) ($($existing.Status))"
  exit 0
}

# 1. Settings.
New-Item -ItemType Directory -Force -Path $SettingsDir | Out-Null
Copy-Item (Join-Path $Package 'vdd_settings.xml') $SettingsDir -Force
New-Item -Force -Path 'HKLM:\SOFTWARE\MikeTheTech\VirtualDisplayDriver' | Out-Null
Set-ItemProperty -Path 'HKLM:\SOFTWARE\MikeTheTech\VirtualDisplayDriver' -Name VDDPATH -Value $SettingsDir
"settings: $SettingsDir\vdd_settings.xml"

# 2. Trust the signer, so the install is silent.
$sig = Get-AuthenticodeSignature $cat
if ($sig.Status -ne 'Valid') { throw "mttvdd.cat signature is $($sig.Status): $($sig.StatusMessage)" }
$cert = $sig.SignerCertificate
$store = New-Object System.Security.Cryptography.X509Certificates.X509Store('TrustedPublisher', 'LocalMachine')
$store.Open('ReadWrite')
$store.Add($cert)
$store.Close()
"trusted publisher: $($cert.Subject) $($cert.Thumbprint)"

# 3. The device, then the driver on it.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class FerretRootDevice {
  [StructLayout(LayoutKind.Sequential)]
  public struct SP_DEVINFO_DATA { public uint cbSize; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }
  [DllImport("setupapi.dll", SetLastError = true)]
  static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid classGuid, IntPtr hwnd);
  [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid classGuid, string description, IntPtr hwnd, uint flags, ref SP_DEVINFO_DATA data);
  [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA data, uint property, byte[] buffer, uint size);
  [DllImport("setupapi.dll", SetLastError = true)]
  static extern bool SetupDiCallClassInstaller(uint function, IntPtr set, ref SP_DEVINFO_DATA data);
  [DllImport("setupapi.dll", SetLastError = true)]
  static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
  [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hardwareId, string infPath, uint flags, out bool reboot);

  const uint DICD_GENERATE_ID = 1, SPDRP_HARDWAREID = 1, DIF_REGISTERDEVICE = 0x19, INSTALLFLAG_FORCE = 1;

  public static string Create(string hardwareId, string infPath) {
    Guid display = new Guid("4d36e968-e325-11ce-bfc1-08002be10318");
    IntPtr set = SetupDiCreateDeviceInfoList(ref display, IntPtr.Zero);
    if (set == new IntPtr(-1)) return "SetupDiCreateDeviceInfoList failed: " + Marshal.GetLastWin32Error();
    try {
      SP_DEVINFO_DATA d = new SP_DEVINFO_DATA();
      d.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
      if (!SetupDiCreateDeviceInfoW(set, "Display", ref display, null, IntPtr.Zero, DICD_GENERATE_ID, ref d))
        return "SetupDiCreateDeviceInfo failed: " + Marshal.GetLastWin32Error();
      // A REG_MULTI_SZ: the ID, then the empty string that ends the list.
      byte[] ids = System.Text.Encoding.Unicode.GetBytes(hardwareId + "\0\0");
      if (!SetupDiSetDeviceRegistryPropertyW(set, ref d, SPDRP_HARDWAREID, ids, (uint)ids.Length))
        return "setting the hardware ID failed: " + Marshal.GetLastWin32Error();
      if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref d))
        return "registering the device failed: " + Marshal.GetLastWin32Error();
    } finally {
      SetupDiDestroyDeviceInfoList(set);
    }
    bool reboot;
    if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hardwareId, infPath, INSTALLFLAG_FORCE, out reboot))
      return "installing the driver failed: " + Marshal.GetLastWin32Error();
    return reboot ? "ok (Windows asks for a reboot)" : "ok";
  }
}
'@
$result = [FerretRootDevice]::Create('Root\MttVDD', (Resolve-Path $inf).Path)
"device: $result"
if (-not $result.StartsWith('ok')) { exit 1 }

Start-Sleep -Seconds 3
Get-PnpDevice -Class Display | ForEach-Object { "  $($_.FriendlyName) | $($_.InstanceId) | $($_.Status)" }
Get-PnpDevice -Class Monitor -PresentOnly | ForEach-Object { "  monitor: $($_.FriendlyName) | $($_.InstanceId)" }
