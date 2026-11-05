$toolchainRoot = "$env:LOCALAPPDATA\usb-monitor-toolchain"
if (-not (Test-Path "$toolchainRoot\msvc\.done")) { throw "Run tools\get-toolchain.ps1 first." }

$kit = '10.0.28000.0'
$sdk = "$toolchainRoot\nuget\microsoft.windows.sdk.cpp\c"
$sdkLibs = "$toolchainRoot\nuget\microsoft.windows.sdk.cpp.x64\c"
$wdk = "$toolchainRoot\nuget\microsoft.windows.wdk.x64\c"
$msvc = (Get-ChildItem "$toolchainRoot\msvc\VC\Tools\MSVC" | Sort-Object Name | Select-Object -Last 1).FullName

$env:PATH = "$msvc\bin\Hostx64\x64;$sdk\bin\$kit\x64;$wdk\bin\$kit\x64;$env:PATH"
$env:INCLUDE = @(
    "$msvc\include",
    "$sdk\Include\$kit\ucrt",
    "$sdk\Include\$kit\um",
    "$sdk\Include\$kit\shared",
    "$sdk\Include\$kit\winrt"
) -join ';'
$env:LIB = @(
    "$msvc\lib\x64",
    "$sdkLibs\ucrt\x64",
    "$sdkLibs\um\x64"
) -join ';'

$inf2cat = "$wdk\bin\$kit\x86\Inf2Cat.exe"
$signtool = "$sdk\bin\$kit\x64\signtool.exe"
