param(
    [string]$Root = "$env:LOCALAPPDATA\usb-monitor-toolchain"
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$kitVersion = '10.0.28000.2526'
$msvcVersion = '14.52'
$channelUrl = 'https://aka.ms/vs/18/stable/channel'
$nugetPackages = 'microsoft.windows.sdk.cpp', 'microsoft.windows.sdk.cpp.x64', 'microsoft.windows.wdk.x64'
$msvcPackages = @(
    "Microsoft.VC.$msvcVersion.Tools.HostX64.TargetX64.base",
    "Microsoft.VC.$msvcVersion.Tools.HostX64.TargetX64.Res.base",
    "Microsoft.VC.$msvcVersion.CRT.Headers.base",
    "Microsoft.VC.$msvcVersion.CRT.x64.Desktop.base",
    "Microsoft.VC.$msvcVersion.CRT.x64.Store.base",
    "Microsoft.VC.$msvcVersion.ASAN.Headers.base"
)

$downloads = Join-Path $Root 'downloads'
New-Item -ItemType Directory -Force $downloads | Out-Null

function Expand-ZipEntries([string]$Archive, [string]$Destination, [string]$Prefix) {
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        foreach ($entry in $zip.Entries) {
            # VSIX entry names are percent-encoded, so "%2B" has to become "+" on disk.
            $name = [Uri]::UnescapeDataString($entry.FullName)
            if (-not $name.StartsWith($Prefix) -or $name.EndsWith('/')) { continue }
            $target = Join-Path $Destination $name.Substring($Prefix.Length)
            New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        }
    } finally {
        $zip.Dispose()
    }
}

foreach ($id in $nugetPackages) {
    $target = Join-Path $Root "nuget\$id"
    if (Test-Path (Join-Path $target '.done')) { continue }
    $archive = Join-Path $downloads "$id.$kitVersion.nupkg"
    if (-not (Test-Path $archive)) {
        Write-Host "download $id $kitVersion"
        Invoke-WebRequest -UseBasicParsing "https://api.nuget.org/v3-flatcontainer/$id/$kitVersion/$id.$kitVersion.nupkg" -OutFile $archive
    }
    Write-Host "extract $id"
    Expand-ZipEntries $archive $target ''
    New-Item -ItemType File (Join-Path $target '.done') | Out-Null
}

$msvcRoot = Join-Path $Root 'msvc'
if (-not (Test-Path (Join-Path $msvcRoot '.done'))) {
    $channel = Invoke-RestMethod $channelUrl
    $manifestItem = $channel.channelItems | Where-Object id -eq 'Microsoft.VisualStudio.Manifests.VisualStudio' | Select-Object -First 1
    $manifestPath = Join-Path $downloads 'VisualStudio.vsman'
    Invoke-WebRequest -UseBasicParsing $manifestItem.payloads[0].url -OutFile $manifestPath
    $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json

    foreach ($id in $msvcPackages) {
        $package = $manifest.packages | Where-Object { $_.id -eq $id -and (-not $_.language -or $_.language -eq 'en-US') } | Select-Object -First 1
        if (-not $package) { throw "$id is not in the Visual Studio channel" }
        foreach ($payload in $package.payloads) {
            $archive = Join-Path $downloads (Split-Path $payload.fileName -Leaf)
            if (-not (Test-Path $archive) -or (Get-FileHash $archive -Algorithm SHA256).Hash -ne $payload.sha256) {
                Write-Host "download $id $($package.version)"
                Invoke-WebRequest -UseBasicParsing $payload.url -OutFile $archive
                if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne $payload.sha256) { throw "SHA-256 mismatch for $archive" }
            }
            Write-Host "extract $id"
            Expand-ZipEntries $archive $msvcRoot 'Contents/'
        }
    }
    New-Item -ItemType File (Join-Path $msvcRoot '.done') | Out-Null
}

Write-Host "toolchain ready: $Root"
