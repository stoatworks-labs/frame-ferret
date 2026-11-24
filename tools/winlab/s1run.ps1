# Runs one command in the signed-in console session (session 1) and prints
# its output. An ssh session is session 0, which has no displays: EnumDisplay-
# Devices returns nothing there and Desktop Duplication cannot start. A
# scheduled task with an interactive logon runs on the user's desktop instead.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File s1run.ps1 -Exe <path> [-Arguments "<args>"] [-Seconds 60]
#
# The task is named FFVD-run, is created for this one run and deleted after.
# It starts in the executable's own folder, not System32.
param(
  [Parameter(Mandatory = $true)][string]$Exe,
  [string]$Arguments = '',
  [int]$Seconds = 60,
  [string]$Log = 'C:\build\ffvd\s1run.log'
)
$ErrorActionPreference = 'Stop'
$name = 'FFVD-run'
$dir = Split-Path -Parent $Exe
if (Test-Path $Log) { Remove-Item $Log -Force }
$marker = 'S1RUN-EXIT'
$cmd = "/c `"`"$Exe`" $Arguments > `"$Log`" 2>&1 & echo $marker %ERRORLEVEL% >> `"$Log`"`""

Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
$action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument $cmd -WorkingDirectory $dir
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds ($Seconds + 30)) -AllowStartIfOnBatteries
Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $name

$deadline = (Get-Date).AddSeconds($Seconds + 20)
$done = $false
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 500
  if ((Test-Path $Log) -and (Select-String -Path $Log -Pattern $marker -Quiet)) { $done = $true; break }
}
if (-not $done) {
  "s1run: still running after $Seconds s; stopping it"
  Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
}
Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
if (Test-Path $Log) { Get-Content $Log }
