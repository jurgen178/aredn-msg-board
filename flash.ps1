
# https://arduino.github.io/arduino-cli/1.5/
# arduino-cli core update-index
#
# WORKFLOW
# 1. Change $Version below for a new firmware release.
# 2. Run this script. It compiles the firmware, creates the release ZIP,
#    updates the web release list, builds the web flasher, and uploads the board.
# 3. Upload the complete contents of web-flasher\dist\ to the web server.
#    Do not upload src, public, node_modules, or the project index.html.


# --- CONFIGURATION ---
# Path to the folder containing arduino-cli.exe (without a trailing backslash)
$CliDir     = "C:\Arduino"
$ArduinoCli = "$CliDir\arduino-cli.exe"
$LibraryDir = "$CliDir\libraries"
$BuildDir   = Join-Path $PSScriptRoot "build"
$Version    = "1.0.0"
$ReleaseRoot = Join-Path $PSScriptRoot "web-flasher\releases"
$ReleaseDirectory = Join-Path $ReleaseRoot "aredn-v$Version"
$ReleaseZip = Join-Path $ReleaseRoot "aredn-v$Version.zip"
$PublicReleaseDirectory = Join-Path $PSScriptRoot "web-flasher\public\releases"
$WebFlasherDirectory = Join-Path $PSScriptRoot "web-flasher"

if (-not (Get-Command node -ErrorAction SilentlyContinue) -or
    -not (Get-Command npm -ErrorAction SilentlyContinue)) {
    Write-Host "Node.js is not installed. Installing the current LTS release..." -ForegroundColor Yellow
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Write-Host "`n[ERROR] winget is not available. Install Node.js LTS manually from https://nodejs.org/ and run this script again." -ForegroundColor Red
        exit 1
    }

    & winget install --id OpenJS.NodeJS.LTS --exact --source winget `
        --silent --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n[ERROR] Node.js installation failed." -ForegroundColor Red
        exit $LASTEXITCODE
    }

    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
        [Environment]::GetEnvironmentVariable("Path", "User")
    if (-not (Get-Command node -ErrorAction SilentlyContinue) -or
        -not (Get-Command npm -ErrorAction SilentlyContinue)) {
        Write-Host "`n[ERROR] Node.js was installed but is not available in this shell. Close and reopen PowerShell, then run the script again." -ForegroundColor Red
        exit 1
    }
}

# Board configuration
# arduino-cli board list

$Board = "arduino:esp32:nano_nora"
$Port  = "COM7"

Write-Host "[1/5] Compiling for Nano ESP32..." -ForegroundColor Cyan
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
& $ArduinoCli compile --libraries $LibraryDir -b $Board --output-dir $BuildDir

if ($LASTEXITCODE -ne 0) {
    Write-Host "`n[ERROR] Compilation failed!" -ForegroundColor Red
    Read-Host "Press Enter to exit..."
    exit $LASTEXITCODE
}

Write-Host "`n[2/5] Creating release package $Version..." -ForegroundColor Cyan
if (Test-Path $ReleaseDirectory) {
    Remove-Item $ReleaseDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $ReleaseDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $ReleaseRoot -Force | Out-Null

$PackageFiles = @(
    "$BuildDir\aredn-service.ino.bin",
    "$BuildDir\aredn-service.ino.bootloader.bin",
    "$BuildDir\aredn-service.ino.partitions.bin"
)
foreach ($PackageFile in $PackageFiles) {
    if (-not (Test-Path $PackageFile)) {
        Write-Host "`n[ERROR] Build artifact missing: $PackageFile" -ForegroundColor Red
        exit 1
    }
    Copy-Item $PackageFile $ReleaseDirectory -Force
}

$Manifest = [ordered]@{
    version = $Version
    board = $Board
    protocol = "dfu"
    files = @(
        [ordered]@{ name = "aredn-service.ino.bin" }
    )
}
$Manifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $ReleaseDirectory "manifest.json")
if (Test-Path $ReleaseZip) {
    Remove-Item $ReleaseZip -Force
}
Compress-Archive -Path (Join-Path $ReleaseDirectory "*") -DestinationPath $ReleaseZip
New-Item -ItemType Directory -Path $PublicReleaseDirectory -Force | Out-Null
Copy-Item $ReleaseZip $PublicReleaseDirectory -Force

$ReleaseIndexPath = Join-Path $PSScriptRoot "web-flasher\public\releases.json"
$ReleaseEntries = @()
if (Test-Path $ReleaseIndexPath) {
    $ReleaseEntries = @(Get-Content $ReleaseIndexPath -Raw | ConvertFrom-Json |
        Where-Object { $_.version -ne $Version })
}
$ReleaseEntries += [ordered]@{
    version = $Version
    label = "AREDN Service $Version"
    url = "./releases/aredn-v$Version.zip"
}
ConvertTo-Json -InputObject $ReleaseEntries -Depth 4 | Set-Content $ReleaseIndexPath
Write-Host "Release created: $ReleaseZip" -ForegroundColor Green

Write-Host "`n[3/5] Installing web flasher dependencies..." -ForegroundColor Cyan
Push-Location $WebFlasherDirectory
& npm install
$NpmInstallExitCode = $LASTEXITCODE
Pop-Location
if ($NpmInstallExitCode -ne 0) {
    Write-Host "`n[ERROR] Installing web flasher dependencies failed!" -ForegroundColor Red
    Read-Host "Press Enter to exit..."
    exit $NpmInstallExitCode
}

Write-Host "`n[4/5] Building web flasher..." -ForegroundColor Cyan
Push-Location $WebFlasherDirectory
& npm run build
$WebBuildExitCode = $LASTEXITCODE
Pop-Location
if ($WebBuildExitCode -ne 0) {
    Write-Host "`n[ERROR] Web flasher build failed!" -ForegroundColor Red
    Read-Host "Press Enter to exit..."
    exit $WebBuildExitCode
}

Write-Host "`n[5/5] Board upload" -ForegroundColor Cyan
$UploadChoice = Read-Host "Upload firmware to board on $Port? [Y/N]"
if ($UploadChoice -match '^(Y|y|Yes|yes)$') {
    Write-Host "Uploading to board ($Port)..." -ForegroundColor Cyan
    & $ArduinoCli upload -p $Port -b $Board --input-dir $BuildDir

    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n[ERROR] Upload failed! Check the USB connection." -ForegroundColor Red
        Read-Host "Press Enter to exit..."
        exit $LASTEXITCODE
    }

    Write-Host "`n[SUCCESS] Firmware compiled, packaged, and uploaded successfully!" -ForegroundColor Green
}
else {
    Write-Host "Upload skipped. Firmware package and web flasher are ready." -ForegroundColor Yellow
}
