$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\toolchain.ps1"

$root = Split-Path $PSScriptRoot
$source = Join-Path $root 'companion'
$out = Join-Path $root 'out\companion'
$obj = Join-Path $root 'out\obj\companion'
New-Item -ItemType Directory -Force $out, $obj | Out-Null

$common = @('/nologo', '/std:c++20', '/EHsc', '/O2', '/MT', '/W4', '/WX', '/external:anglebrackets', '/external:W0', '/Zi',
    "/external:I$root\third_party\nvenc", '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/D_CRT_SECURE_NO_WARNINGS')

$compile = $common + @('/c', "/Fo$obj\", "/Fd$obj\vc.pdb")
$compile += Get-ChildItem $source -Filter *.cpp | Select-Object -ExpandProperty FullName
& cl.exe @compile
if ($LASTEXITCODE -ne 0) { throw 'companion compile failed' }

$libraries = 'd3d11.lib', 'dxgi.lib', 'mfplat.lib', 'mfuuid.lib', 'ole32.lib', 'oleaut32.lib', 'ws2_32.lib', 'user32.lib', 'advapi32.lib', 'avrt.lib', 'shcore.lib', 'setupapi.lib'
$link = @('/nologo', '/SUBSYSTEM:CONSOLE', "/OUT:$out\aum-companion.exe", "/PDB:$obj\aum-companion.pdb", '/DEBUG', '/OPT:REF', '/OPT:ICF', '/INCREMENTAL:NO')
$link += $libraries
$link += Get-ChildItem $obj -Filter *.obj | Select-Object -ExpandProperty FullName
& link.exe @link
if ($LASTEXITCODE -ne 0) { throw 'companion link failed' }

$testObj = Join-Path $root 'out\obj\tests'
New-Item -ItemType Directory -Force $testObj | Out-Null
foreach ($test in @{ name = 'edid'; sources = @('edid.cpp') }, @{ name = 'phone_layer'; sources = @('phone_layer.cpp') }, @{ name = 'phone_cadence'; sources = @('phone_cadence.cpp') }, @{ name = 'pending_frame_slot'; sources = @() }, @{ name = 'panel_rate_picker'; sources = @() }, @{ name = 'stream_connection'; sources = @('stream_connection.cpp') }) {
    $testCompile = $common + @("/Fo$testObj\", "/Fd$testObj\vc.pdb", "/Fe:$out\$($test.name)_test.exe", "$source\tests\$($test.name)_test.cpp")
    $testCompile += $test.sources | ForEach-Object { "$source\$_" }
    $testCompile += 'ws2_32.lib'
    & cl.exe @testCompile
    if ($LASTEXITCODE -ne 0) { throw "$($test.name) test compile failed" }
    & "$out\$($test.name)_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "$($test.name) tests failed" }
}

Write-Host "companion ready: $out\aum-companion.exe"
