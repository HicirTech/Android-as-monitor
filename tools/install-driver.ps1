#Requires -RunAsAdministrator
<#
.SYNOPSIS
Installs the display driver: trusts its local test certificate, adds the driver package, creates the virtual adapter.

.DESCRIPTION
Run from an elevated PowerShell. Rerun after every driver rebuild, with the companion stopped.
Secure Boot, BitLocker and test signing are not touched. tools\uninstall-driver.ps1 undoes all of it.
#>
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot
$package = Join-Path $root 'out\driver'
$inf = Join-Path $package 'AndroidUsbMonitor.inf'
$certificate = Join-Path $package 'AndroidUsbMonitor.cer'
$companion = Join-Path $root 'out\companion\aum-companion.exe'

foreach ($required in $inf, $certificate, $companion) {
    if (-not (Test-Path $required)) { throw "$required is missing: run tools\build-driver.ps1 and tools\build-companion.ps1 first" }
}

Write-Host 'trusting the local test certificate (LocalMachine Root and TrustedPublisher)'
Import-Certificate -FilePath $certificate -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $certificate -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

Write-Host 'adding the driver package'
& pnputil.exe /add-driver $inf /install
# 259 is ERROR_NO_MORE_ITEMS: the package was added, but no device needed it yet, which is normal on a first install.
$ERROR_SUCCESS_REBOOT_REQUIRED = 3010
if ($LASTEXITCODE -eq $ERROR_SUCCESS_REBOOT_REQUIRED) { Write-Host 'Windows asks for a reboot before this build takes effect' }
elseif ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) { throw "pnputil failed with exit code $LASTEXITCODE" }

$device = Get-PnpDevice -InstanceId 'SWD\ANDROIDUSBMONITOR\*' -ErrorAction SilentlyContinue
if ($device) {
    Write-Host 'restarting the virtual adapter so that it loads this build'
    & pnputil.exe /restart-device $device.InstanceId
} else {
    Write-Host 'creating the virtual adapter'
    & $companion install-device
    if ($LASTEXITCODE -ne 0) { throw "install-device failed with exit code $LASTEXITCODE" }
}

Get-PnpDevice -InstanceId 'SWD\ANDROIDUSBMONITOR\*' | Format-Table Status, Class, FriendlyName, InstanceId -AutoSize
Write-Host 'driver events:'
& "$PSScriptRoot\driver-events.ps1" -Count 5
