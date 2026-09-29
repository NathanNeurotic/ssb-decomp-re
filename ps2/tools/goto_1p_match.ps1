# Boot the ELF in PCSX2 and drive the menus into a 1P Game match as Mario
# (title -> Mode Select -> 1P Game -> character select -> Mario -> Start).
# Uses the default PCSX2 keyboard bindings (see pcsx2_press.ps1).
# Usage: powershell -File ps2/tools/goto_1p_match.ps1 [-SettleSeconds 20]
param([int]$SettleSeconds = 20)
$t = $PSScriptRoot
function Press([string]$k, [int]$ms = 200) {
    powershell -ExecutionPolicy Bypass -File "$t\pcsx2_press.ps1" -Keys $k -HoldMs $ms
}
powershell -ExecutionPolicy Bypass -File "$t\run_pcsx2.ps1" -Seconds 22 | Out-Null
foreach ($k in "Return", "Return", "K", "K") { Press $k; Start-Sleep 3 }
Press "W" 900   # cursor up past the portraits
Press "S" 200   # back down onto the top row
Press "D" 250
Press "A" 150   # Mario
Press "K" 150   # select
Start-Sleep 2
Press "Return"  # start the match
Start-Sleep $SettleSeconds
