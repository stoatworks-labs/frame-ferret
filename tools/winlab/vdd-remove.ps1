# Undoes vdd-install.ps1 on a test machine: the Root\MttVDD device, the driver
# package, the phantom monitors it left, the trusted-publisher certificate, the
# settings folder and the registry key. Leaves the machine as it was before.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File vdd-remove.ps1 -Package <dir>
#
# <dir> is the same unpacked VirtualDisplayDriver folder, read only to find
# which certificate to take back out of TrustedPublisher.
param(
  [Parameter(Mandatory = $true)][string]$Package,
  [string]$SettingsDir = 'C:\VirtualDisplayDriver'
)
$ErrorActionPreference = 'Continue'

Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
  Where-Object { $_.HardwareID -contains 'Root\MttVDD' } |
  ForEach-Object {
    "removing device $($_.InstanceId)"
    pnputil /remove-device "$($_.InstanceId)"
  }

# The package's published name (oemNN.inf) is whatever Windows assigned.
$lines = pnputil /enum-drivers
for ($i = 0; $i -lt $lines.Count; $i++) {
  if ($lines[$i] -match 'Original Name:\s+mttvdd\.inf') {
    for ($j = $i; $j -ge [Math]::Max(0, $i - 3); $j--) {
      if ($lines[$j] -match 'Published Name:\s+(oem\d+\.inf)') {
        "deleting driver package $($Matches[1])"
        pnputil /delete-driver $Matches[1] /uninstall /force
        break
      }
    }
  }
}

# Windows keeps the VDD monitors it has seen as phantom devices (winlab had
# five after a session of reloads and restarts); they outlive the driver.
Get-PnpDevice -Class Monitor -ErrorAction SilentlyContinue |
  Where-Object { $_.InstanceId -like 'DISPLAY\MTT1337\*' } |
  ForEach-Object {
    "removing monitor $($_.InstanceId)"
    pnputil /remove-device "$($_.InstanceId)" | Out-Null
  }

$cat = Join-Path $Package 'mttvdd.cat'
if (Test-Path $cat) {
  $thumb = (Get-AuthenticodeSignature $cat).SignerCertificate.Thumbprint
  if ($thumb) {
    "removing trusted publisher $thumb"
    certutil -delstore TrustedPublisher $thumb | Out-Null
  }
}

if (Test-Path $SettingsDir) { Remove-Item -Recurse -Force $SettingsDir; "removed $SettingsDir" }
if (Test-Path 'HKLM:\SOFTWARE\MikeTheTech') { Remove-Item -Recurse -Force 'HKLM:\SOFTWARE\MikeTheTech'; "removed HKLM\SOFTWARE\MikeTheTech" }

Get-PnpDevice -Class Display -PresentOnly | ForEach-Object { "  left: $($_.FriendlyName) | $($_.Status)" }
