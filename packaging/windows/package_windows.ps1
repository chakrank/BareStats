# BareStats Automated Windows Release Packaging Script
# Usage: powershell -ExecutionPolicy Bypass -File packaging/windows/package_windows.ps1

param(
    [string]$BuildDir = "build",
    [string]$Version = "1.3.0"
)

$ErrorActionPreference = "Stop"

Write-Output "Validating build artifacts in '$BuildDir'..."
$exePath = Join-Path $BuildDir "BareStats.exe"
if (-not (Test-Path $exePath)) {
    Write-Error "BareStats.exe not found in $BuildDir. Please compile the Release binary first."
    exit 1
}

$releaseName = "BareStats-v$Version-Windows-x64"
$stagingDir = Join-Path $BuildDir $releaseName
$zipPath = Join-Path $BuildDir "$releaseName.zip"

Write-Output "Creating staging directory: $stagingDir"
if (Test-Path $stagingDir) {
    Remove-Item -Recurse -Force $stagingDir
}
New-Item -ItemType Directory -Path $stagingDir | Out-Null

Copy-Item $exePath -Destination $stagingDir
Copy-Item "config.example.ini" -Destination $stagingDir
Copy-Item "LICENSE" -Destination $stagingDir
Copy-Item "README.md" -Destination $stagingDir

Write-Output "Compressing archive to: $zipPath"
if (Test-Path $zipPath) {
    Remove-Item -Force $zipPath
}
Compress-Archive -Path "$stagingDir/*" -DestinationPath $zipPath

$zipHash = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash
$exeHash = (Get-FileHash -Path $exePath -Algorithm SHA256).Hash

Write-Output "Release Package Created Successfully:"
Write-Output "  Archive:  $zipPath"
Write-Output "  SHA-256:  $zipHash"
Write-Output "  Binary:   $exePath"
Write-Output "  SHA-256:  $exeHash"

"$exeHash  BareStats.exe" | Out-File -FilePath (Join-Path $BuildDir "BareStats.exe.sha256") -Encoding ascii
"$zipHash  $releaseName.zip" | Out-File -FilePath (Join-Path $BuildDir "$releaseName.zip.sha256") -Encoding ascii
