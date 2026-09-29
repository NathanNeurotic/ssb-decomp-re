# Press keys in the PCSX2 game window for testing (default Pad1 keyboard
# bindings: Return=Start, K=Cross, J=Square, L=Circle, I=Triangle,
# arrows=D-pad, W/A/S/D=left stick, Q/E=L1/R1, 1/3=L2/R2, 4=R3, Backspace=Select).
# Usage: powershell -File ps2/tools/pcsx2_press.ps1 -Keys "Return","K" [-HoldMs 150] [-GapMs 400]
#        a key may be "K+Down" to hold several together.
param(
    [string[]]$Keys,
    [int]$HoldMs = 150,
    [int]$GapMs = 400
)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Pcsx2Keys {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
"@
$vk = @{
    "Return" = 0x0D; "Backspace" = 0x08; "Up" = 0x26; "Down" = 0x28; "Left" = 0x25; "Right" = 0x27;
    "1" = 0x31; "2" = 0x32; "3" = 0x33; "4" = 0x34
}
foreach ($c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ".ToCharArray()) { $vk["$c"] = [int][char]$c }
$proc = Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like "ssb64*" } | Select-Object -First 1
if (-not $proc) { Write-Error "PCSX2 game window not found"; exit 1 }
[Pcsx2Keys]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
$extended = @(0x25, 0x26, 0x27, 0x28)
foreach ($k in $Keys) {
    $codes = $k.Split("+") | ForEach-Object { $vk[$_] }
    foreach ($c in $codes) {
        $f = 0; if ($extended -contains $c) { $f = 1 }
        [Pcsx2Keys]::keybd_event([byte]$c, [byte][Pcsx2Keys]::MapVirtualKey($c, 0), $f, [UIntPtr]::Zero)
    }
    Start-Sleep -Milliseconds $HoldMs
    foreach ($c in $codes) {
        $f = 2; if ($extended -contains $c) { $f = 3 }
        [Pcsx2Keys]::keybd_event([byte]$c, [byte][Pcsx2Keys]::MapVirtualKey($c, 0), $f, [UIntPtr]::Zero)
    }
    Start-Sleep -Milliseconds $GapMs
}
