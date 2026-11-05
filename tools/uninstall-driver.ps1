#Requires -RunAsAdministrator
<#
.SYNOPSIS
Removes everything tools\install-driver.ps1 added: the virtual adapter, the driver package and the certificate.
#>
$ErrorActionPreference = 'Stop'

foreach ($device in Get-PnpDevice -InstanceId 'ROOT\ANDROIDUSBMONITOR\*' -ErrorAction SilentlyContinue) {
    Write-Host "removing device $($device.InstanceId)"
    & pnputil.exe /remove-device $device.InstanceId
}

$packages = Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceName LIKE 'Android USB Monitor%'" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty InfName -Unique
$published = & pnputil.exe /enum-drivers
$current = $null
foreach ($line in $published) {
    if ($line -match '^\s*Published Name:\s*(oem\d+\.inf)') { $current = $Matches[1] }
    if ($line -match '^\s*Original Name:\s*androidusbmonitor\.inf' -and $current) { $packages = @($packages) + $current }
}
foreach ($package in ($packages | Where-Object { $_ } | Select-Object -Unique)) {
    Write-Host "deleting driver package $package"
    & pnputil.exe /delete-driver $package /uninstall /force
}

foreach ($store in 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher') {
    Get-ChildItem $store | Where-Object Subject -eq 'CN=Android USB Monitor Local Test' | ForEach-Object {
        Write-Host "removing certificate $($_.Thumbprint) from $store"
        Remove-Item $_.PSPath
    }
}
Write-Host 'done'
