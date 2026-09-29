# Ask PCSX2 for a screenshot of the emulated display (its F8 "Save
# Screenshot" hotkey) and copy the newest file from its snaps folder to -Out.
# Only the PCSX2 game window receives the key press.
# Usage: powershell -File ps2/tools/screenshot_pcsx2.ps1 -Out shot.png [-Pcsx2Dir <dir>]
param(
    [string]$Out = "$env:TEMP\pcsx2_shot.png",
    [string]$Pcsx2Dir = $env:PCSX2_DIR
)
if (-not $Pcsx2Dir) { $Pcsx2Dir = "E:\PCSX2-MCP-v1.0.0-win64\PCSX2-MCP-v1.0.0-win64" }
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Pcsx2Win {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
$snaps = Join-Path $Pcsx2Dir "snaps"
New-Item -ItemType Directory -Force $snaps | Out-Null
$before = Get-Date
$proc = Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like "ssb64*" } | Select-Object -First 1
if (-not $proc) { Write-Error "PCSX2 game window not found"; exit 1 }
[Pcsx2Win]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 400
[System.Windows.Forms.SendKeys]::SendWait("{F8}")
for ($i = 0; $i -lt 60; $i++) {   # up to 15 s: PCSX2 can be slow to encode
    Start-Sleep -Milliseconds 250
    $shot = Get-ChildItem $snaps -Filter *.png -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $before } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($shot) {
        # PCSX2 may still be writing the PNG: retry until it can be read.
        for ($j = 0; $j -lt 20; $j++) {
            try { Copy-Item -LiteralPath $shot.FullName -Destination $Out -Force -ErrorAction Stop; Write-Output $Out; exit 0 }
            catch { Start-Sleep -Milliseconds 250 }
        }
    }
}
Write-Error "no screenshot produced"
exit 1
