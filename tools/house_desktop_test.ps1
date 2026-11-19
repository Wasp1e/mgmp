# Bounded desktop actions for the authorized local House test. Only targets
# an explicitly named Mewgenics PID; never clicks an unrelated application.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][int]$GamePid,
    [ValidateSet('Screenshot','Click','Move','Down','Up','Key','Drag')]
    [string]$Action = 'Screenshot',
    [int]$X = 0, [int]$Y = 0,
    [int]$ToX = 0, [int]$ToY = 0,
    [ValidateSet('ESC','ENTER','SPACE','F1')][string]$Key = 'ESC',
    [ValidateRange(1,60)][int]$Steps = 20,
    [ValidateRange(0,3000)][int]$HoldMs = 600,
    [int]$ReceiverPid = 0,
    [switch]$ReturnToStart,
    [string]$Output
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
if (-not ('HouseDesktop' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class HouseDesktop {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref Point p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a,uint b,bool attach);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int command);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point p);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,UIntPtr e);
    [DllImport("user32.dll")] public static extern void keybd_event(byte k,byte s,uint f,UIntPtr e);
}
'@
}
$game = Get-Process -Id $GamePid
if ($game.ProcessName -ne 'Mewgenics' -or $game.MainWindowHandle -eq [IntPtr]::Zero) {
    throw 'Target must be a visible Mewgenics window.'
}
$window = $game.MainWindowHandle
if ($Action -ne 'Screenshot') {
[HouseDesktop]::SetForegroundWindow($window) | Out-Null
if ([HouseDesktop]::GetForegroundWindow() -ne $window) {
    $current = [HouseDesktop]::GetCurrentThreadId()
    $foreground = [HouseDesktop]::GetWindowThreadProcessId([HouseDesktop]::GetForegroundWindow(),[IntPtr]::Zero)
    $attached = [HouseDesktop]::AttachThreadInput($current,$foreground,$true)
    try {
        [HouseDesktop]::ShowWindow($window,5) | Out-Null
        [HouseDesktop]::BringWindowToTop($window) | Out-Null
        [HouseDesktop]::SetForegroundWindow($window) | Out-Null
    } finally {
        if ($attached) { [HouseDesktop]::AttachThreadInput($current,$foreground,$false) | Out-Null }
    }
}
Start-Sleep -Milliseconds 120
if ([HouseDesktop]::GetForegroundWindow() -ne $window) { throw 'Could not focus the game; no input sent.' }
}
$rect = New-Object HouseDesktop+Rect
$origin = New-Object HouseDesktop+Point
if (-not [HouseDesktop]::GetClientRect($window,[ref]$rect) -or
    -not [HouseDesktop]::ClientToScreen($window,[ref]$origin)) { throw 'Could not read game bounds.' }
function Move-To([int]$px,[int]$py) {
    if ($px -lt 0 -or $py -lt 0 -or $px -ge $rect.Right -or $py -ge $rect.Bottom) {
        throw "Point outside game client: $px,$py"
    }
    if ([HouseDesktop]::GetForegroundWindow() -ne $window) { throw 'Focus left the game; stopped.' }
    [HouseDesktop]::SetCursorPos($origin.X+$px,$origin.Y+$py) | Out-Null
}
function Check-Cursor([int]$px,[int]$py) {
    $actual = New-Object HouseDesktop+Point
    [HouseDesktop]::GetCursorPos([ref]$actual) | Out-Null
    if ([Math]::Abs($actual.X-($origin.X+$px)) -gt 3 -or
        [Math]::Abs($actual.Y-($origin.Y+$py)) -gt 3) {
        throw "Cursor moved away from test target ($($actual.X),$($actual.Y)); stopped input."
    }
    if ([HouseDesktop]::GetForegroundWindow() -ne $window) { throw 'Game lost focus; stopped input.' }
}
function Mouse([uint32]$flags) { [HouseDesktop]::mouse_event($flags,0,0,0,[UIntPtr]::Zero) }
function Capture-Window([int]$CapturePid,[string]$File) {
    if (-not $File) { return }
    $p = Get-Process -Id $CapturePid
    if ($p.ProcessName -ne 'Mewgenics') { throw 'Capture target is not the game.' }
    $r = New-Object HouseDesktop+Rect
    $o = New-Object HouseDesktop+Point
    if (-not [HouseDesktop]::GetClientRect($p.MainWindowHandle,[ref]$r) -or
        -not [HouseDesktop]::ClientToScreen($p.MainWindowHandle,[ref]$o)) { throw 'Capture bounds failed.' }
    $bitmap = New-Object System.Drawing.Bitmap $r.Right,$r.Bottom
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($o.X,$o.Y,0,0,$bitmap.Size)
        $bitmap.Save([IO.Path]::GetFullPath($File))
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
function Capture { Capture-Window $GamePid $Output }
switch ($Action) {
    'Click' { Move-To $X $Y; Start-Sleep -Milliseconds 100; Check-Cursor $X $Y; Mouse 2; Start-Sleep -Milliseconds 100; Mouse 4 }
    'Move' { Move-To $X $Y; Check-Cursor $X $Y }
    'Down' { Move-To $X $Y; Start-Sleep -Milliseconds 100; Check-Cursor $X $Y; Mouse 2 }
    'Up' { Mouse 4 }
    'Key' {
        $vk = switch ($Key) { 'ESC' {27} 'ENTER' {13} 'SPACE' {32} 'F1' {112} }
        [HouseDesktop]::keybd_event($vk,0,0,[UIntPtr]::Zero)
        Start-Sleep -Milliseconds 80
        [HouseDesktop]::keybd_event($vk,0,2,[UIntPtr]::Zero)
    }
    'Drag' {
        Move-To $X $Y
        Start-Sleep -Milliseconds 100
        Check-Cursor $X $Y
        Mouse 2
        try {
            for ($i=1; $i -le $Steps; $i++) {
                Move-To ([int]($X+($ToX-$X)*$i/$Steps)) ([int]($Y+($ToY-$Y)*$i/$Steps))
                Start-Sleep -Milliseconds 60
                Check-Cursor ([int]($X+($ToX-$X)*$i/$Steps)) ([int]($Y+($ToY-$Y)*$i/$Steps))
            }
            Start-Sleep -Milliseconds $HoldMs
            Capture
            if ($ReceiverPid -and $Output) { Capture-Window $ReceiverPid ($Output + '.peer.png') }
            if ($ReturnToStart) {
                Move-To $X $Y
                Start-Sleep -Milliseconds 200
                Check-Cursor $X $Y
            }
        } finally { Mouse 4 }
    }
}
if ($Action -ne 'Drag') {
    Start-Sleep -Milliseconds $HoldMs
    Capture
}
Write-Output "Game PID $GamePid client=$($rect.Right)x$($rect.Bottom) origin=$($origin.X),$($origin.Y) action=$Action screenshot=$Output"
