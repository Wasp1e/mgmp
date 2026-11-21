# Build and restart the two-instance local test, then put the host on the
# primary monitor and the client on the next monitor.
#
#   .\tools\restart_test.ps1         # asks before closing running games
#   .\tools\restart_test.ps1 -Force  # close running games without asking
#   .\tools\restart_test.ps1 -SplitPrimary  # host left, client right
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
    [switch]$HouseCatSync,
    [switch]$SplitPrimary,
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

$netTestArgs = @{ BuildDir = $BuildDir; Config = $Config }
foreach ($option in 'HousePositions', 'HouseWalk', 'HouseActions', 'HouseDrag', 'HouseDragSync', 'HouseCatSync') {
    if ($PSBoundParameters.ContainsKey($option)) {
        $netTestArgs[$option] = $PSBoundParameters[$option]
    }
}
& (Join-Path $PSScriptRoot 'net_test.ps1') @netTestArgs

Add-Type -AssemblyName System.Windows.Forms
if (-not ('MgmpTestWindow' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MgmpTestWindow {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetWindowPos(IntPtr window, IntPtr after,
        int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")]
    public static extern bool IsZoomed(IntPtr window);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
}
'@
}

$deadline = (Get-Date).AddSeconds(25)
do {
    $games = @(Get-Process Mewgenics -ErrorAction SilentlyContinue | Sort-Object StartTime)
    $visible = @($games | Where-Object {
        $_.MainWindowHandle -ne [IntPtr]::Zero -and $_.MainWindowTitle -like 'Mewgenics*'
    })
    if ($games.Count -eq 2 -and $visible.Count -eq 2) { break }
    Start-Sleep -Milliseconds 500
} while ((Get-Date) -lt $deadline)
if ($games.Count -ne 2 -or $visible.Count -ne 2) {
    Write-Warning "Expected two game windows, found $($visible.Count); windows were not moved."
    return
}

# Use physical monitor and window coordinates even when the displays have
# different scales.
$dpi = [MgmpTestWindow]::SetThreadDpiAwarenessContext([IntPtr](-4))
try {
    $screens = @([System.Windows.Forms.Screen]::AllScreens | Sort-Object { -not $_.Primary }, { $_.Bounds.X }, { $_.Bounds.Y })
    if ($screens.Count -lt 2) { $SplitPrimary = $true }
    for ($i = 0; $i -lt 2; ++$i) {
        $window = $games[$i].MainWindowHandle
        $role = if ($i -eq 0) { 'host' } else { 'client' }
        $x = 0
        $width = 0
        $height = 0
        $flags = 0x15 # SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
        if ($SplitPrimary) {
            $area = $screens[0].WorkingArea
            $half = $area.Width -shr 1
            $x = $area.Left + $i * $half
            $width = if ($i -eq 0) { $half } else { $area.Width - $half }
            $height = $area.Height
            $flags = 0x14 # Allow resizing
            if ([MgmpTestWindow]::IsZoomed($window)) { [MgmpTestWindow]::ShowWindow($window, 9) | Out-Null }
        } else {
            $area = $screens[$i].WorkingArea
            $x = $area.Left
        }
        if (-not [MgmpTestWindow]::SetWindowPos($window, [IntPtr]::Zero, $x, $area.Top, $width, $height, $flags)) {
            Write-Warning "Could not move $role window."
            continue
        }
        $rect = New-Object 'MgmpTestWindow+Rect'
        if ([MgmpTestWindow]::GetWindowRect($window, [ref]$rect)) {
            Write-Host "$role window on $($area.X),$($area.Y): $($rect.Left),$($rect.Top) $($rect.Right - $rect.Left)x$($rect.Bottom - $rect.Top)"
        }
    }
} finally {
    if ($dpi -ne [IntPtr]::Zero) { [MgmpTestWindow]::SetThreadDpiAwarenessContext($dpi) | Out-Null }
}
