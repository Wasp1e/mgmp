# Build and restart the two-instance local test, then put the host on the
# primary monitor and the client on the next monitor.
#
#   .\tools\restart_test.ps1         # asks before closing running games
#   .\tools\restart_test.ps1 -Force  # close running games without asking
#
# Both instances use the real Mewgenics save directory. Close the games
# normally first if you need to make sure the current session is saved.
[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$HousePositions,
    [switch]$HouseWalk,
    [switch]$HouseActions,
    [switch]$HouseDrag,
    [switch]$HouseDragSync,
    [string]$BuildDir = 'build',
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$buildPath = Join-Path $root $BuildDir

# Do not close a working session for a build that fails.
Write-Host '==> Building the test version' -ForegroundColor Cyan
& cmake --build $buildPath --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE; games were left running." }

$running = @(Get-Process Mewgenics -ErrorAction SilentlyContinue)
if ($running.Count -gt 0 -and -not $Force) {
    Write-Host "The test script will forcibly close ALL $($running.Count) running Mewgenics instance(s)." -ForegroundColor Yellow
    Write-Host 'Unsaved progress may be lost. Close them normally instead if needed.' -ForegroundColor Yellow
    $answer = Read-Host 'Continue? [y/N]'
    if ($answer -notmatch '^(y|yes)$') {
        Write-Host 'Cancelled; games were left running.'
        return
    }
}

& (Join-Path $PSScriptRoot 'net_test.ps1') -BuildDir $BuildDir -Config $Config -HousePositions:$HousePositions -HouseWalk:$HouseWalk -HouseActions:$HouseActions -HouseDrag:$HouseDrag -HouseDragSync:$HouseDragSync

Add-Type -AssemblyName System.Windows.Forms
if (-not ('MgmpTestWindow' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MgmpTestWindow {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetWindowPos(IntPtr window, IntPtr after,
        int x, int y, int width, int height, uint flags);
}
'@
}

$screens = @([System.Windows.Forms.Screen]::AllScreens | Sort-Object { -not $_.Primary }, { $_.Bounds.X }, { $_.Bounds.Y })
$games = @(Get-Process Mewgenics -ErrorAction SilentlyContinue | Sort-Object StartTime)
if ($games.Count -ne 2) {
    Write-Warning "Expected two game processes, found $($games.Count); windows were not moved."
    return
}
if ($screens.Count -lt 2) {
    Write-Warning 'Only one display was found; windows were not moved.'
    return
}

for ($i = 0; $i -lt 2; ++$i) {
    $game = $games[$i]
    $role = if ($i -eq 0) { 'host' } else { 'client' }
    if ($game.MainWindowHandle -eq [IntPtr]::Zero) {
        Write-Warning "No window found for $role (PID $($game.Id)); move it manually."
        continue
    }
    $area = $screens[$i].WorkingArea
    # SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE: preserve the game's size
    # and do not steal focus while putting each window on its monitor.
    if ([MgmpTestWindow]::SetWindowPos($game.MainWindowHandle, [IntPtr]::Zero,
            $area.X, $area.Y, 0, 0, 0x15)) {
        Write-Host "$role window moved to $($screens[$i].DeviceName)."
    } else {
        Write-Warning "Could not move $role window; move it manually."
    }
}
