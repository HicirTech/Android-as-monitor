$ErrorActionPreference = 'Stop'

$project =Join-Path (Split-Path $PSScriptRoot) 'android'
Push-Location $project
try {
    & .\gradlew.bat --no-daemon testAppLogic assembleRelease @args
    if ($LASTEXITCODE -ne 0) { throw 'gradle build failed' }
} finally {
    Pop-Location
}
Write-Host "apk ready: $project\app\build\outputs\apk\release\app-release.apk"
