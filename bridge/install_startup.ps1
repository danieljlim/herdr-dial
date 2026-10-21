# Starts the bridge at Windows sign-in, with no console window (output goes to logs\bridge.log).
#
#   powershell -ExecutionPolicy Bypass -File bridge\install_startup.ps1             # add and start it now
#   powershell -ExecutionPolicy Bypass -File bridge\install_startup.ps1 -Uninstall  # remove it
#
# Extra bridge options can go in -Arguments, e.g. -Arguments "--ble-address AA:BB:CC:DD:EE:FF".

param(
    [switch]$Uninstall,
    [string]$Arguments = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$shortcut = Join-Path ([Environment]::GetFolderPath("Startup")) "SpeedDial bridge.lnk"

if ($Uninstall) {
    Remove-Item $shortcut -ErrorAction SilentlyContinue
    Write-Host "Removed $shortcut. Quit the running bridge from its tray icon."
    exit 0
}

$pythonw = Join-Path $repo ".venv\Scripts\pythonw.exe"
if (-not (Test-Path $pythonw)) {
    throw "No virtualenv at $repo\.venv. Run the setup steps in README.md first."
}

$script = Join-Path $repo "bridge\speeddial_bridge.py"
$link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
$link.TargetPath = $pythonw
$link.Arguments = "`"$script`" $Arguments".Trim()
$link.WorkingDirectory = $repo
$link.Description = "SpeedDial bridge"
$link.Save()
Write-Host "Added $shortcut"

# The bridge refuses to start twice, so this is harmless if it's already running.
Start-Process -FilePath $pythonw -ArgumentList $link.Arguments -WorkingDirectory $repo
Write-Host "Started the bridge (log: $repo\logs\bridge.log)"
