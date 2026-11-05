$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"

$root = Split-Path $PSScriptRoot
$source = Join-Path $root 'driver'
$out = Join-Path $root 'out\driver'
$obj = Join-Path $root 'out\obj\driver'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out, $obj | Out-Null

$umdf = '2.25'
$iddcx = '1.4'
$includes = @(
    "$wdk\Include\$kit\um\iddcx\$iddcx",
    "$wdk\Include\wdf\umdf\$umdf",
    "$wdk\Include\$kit\um",
    "$wdk\Include\$kit\shared"
)
$defines = 'UMDF_DRIVER', 'UMDF_VERSION_MAJOR=2', 'UMDF_VERSION_MINOR=25', 'UMDF_USING_NTSTATUS', 'IDDCX_VERSION_MAJOR=1', 'IDDCX_VERSION_MINOR=4',
    'IDDCX_MINIMUM_VERSION_REQUIRED=4', '_UNICODE', 'UNICODE', '_WIN64', '_AMD64_', 'AMD64', '_WINDLL'

$compile = @('/nologo', '/std:c++20', '/EHsc', '/O2', '/MT', '/W4', '/WX', '/external:anglebrackets', '/external:W0', '/c', "/Fo$obj\", "/Fd$obj\vc.pdb", '/Zi')
$compile += $defines | ForEach-Object { "/D$_" }
$compile += $includes | ForEach-Object { "/I$_" }
$compile += Get-ChildItem $source -Filter *.cpp | Select-Object -ExpandProperty FullName
& cl.exe @compile
if ($LASTEXITCODE -ne 0) { throw 'driver compile failed' }

$link = @(
    '/nologo', '/DLL', '/SUBSYSTEM:WINDOWS', "/OUT:$out\AndroidUsbMonitor.dll", "/PDB:$obj\AndroidUsbMonitor.pdb", "/IMPLIB:$obj\AndroidUsbMonitor.lib", '/DEBUG',
    '/OPT:REF', '/OPT:ICF', '/INCREMENTAL:NO',
    "$wdk\Lib\wdf\umdf\x64\$umdf\WdfDriverStubUm.lib",
    "$wdk\Lib\$kit\um\x64\iddcx\$iddcx\iddcxstub.lib",
    'ntdll.lib', 'mincore.lib', 'avrt.lib', 'd3d11.lib', 'dxgi.lib', 'advapi32.lib', 'user32.lib'
)
$link += Get-ChildItem $obj -Filter *.obj | Select-Object -ExpandProperty FullName
& link.exe @link
if ($LASTEXITCODE -ne 0) { throw 'driver link failed' }

# PnP only replaces an installed driver with a strictly newer DriverVer, so every build gets its own.
$now = Get-Date
$driverVer = '{0},1.0.{1}.{2}' -f $now.ToString('MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture), [int]($now - [datetime]'2026-01-01').TotalDays, [int]$now.TimeOfDay.TotalMinutes
(Get-Content "$source\AndroidUsbMonitor.inf" -Raw).Replace('@DRIVER_VER@', $driverVer) | Set-Content "$out\AndroidUsbMonitor.inf" -Encoding Ascii -NoNewline

$subject = 'CN=Android USB Monitor Local Test'
$certificate = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date).AddDays(7) } | Select-Object -First 1
if (-not $certificate) {
    $certificate = New-SelfSignedCertificate -Subject $subject -Type CodeSigningCert -KeyExportPolicy NonExportable `
        -KeyUsage DigitalSignature -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(1)
    Write-Host "created signing certificate $($certificate.Thumbprint)"
}
Export-Certificate -Cert $certificate -FilePath "$out\AndroidUsbMonitor.cer" -Force | Out-Null

& $signtool sign /q /fd SHA256 /sha1 $certificate.Thumbprint /s My "$out\AndroidUsbMonitor.dll"
if ($LASTEXITCODE -ne 0) { throw 'signing the driver dll failed' }

Remove-Item "$out\AndroidUsbMonitor.cat" -ErrorAction SilentlyContinue
& $inf2cat "/driver:$out" '/os:10_X64' '/uselocaltime' | Select-Object -Last 3
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed' }

& $signtool sign /q /fd SHA256 /sha1 $certificate.Thumbprint /s My "$out\AndroidUsbMonitor.cat"
if ($LASTEXITCODE -ne 0) { throw 'signing the catalog failed' }

Write-Host "driver package ready: $out (DriverVer $driverVer)"
