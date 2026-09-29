# Boot ps2/build/bin/ssb64.elf in PCSX2 (host: filesystem) and print the
# port's log lines. Usage: powershell -File ps2/tools/run_pcsx2.ps1 [-Seconds 20] [-Pcsx2Dir <dir>]
param(
    [int]$Seconds = 20,
    [string]$Pcsx2Dir = $env:PCSX2_DIR,
    [string]$Pattern = "ssb64\]|PANIC|TLB Miss"
)
if (-not $Pcsx2Dir) { $Pcsx2Dir = "E:\PCSX2-MCP-v1.0.0-win64\PCSX2-MCP-v1.0.0-win64" }
$elf = (Resolve-Path "$PSScriptRoot\..\build\bin\ssb64.elf").Path
Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
$log = Join-Path $Pcsx2Dir "logs\emulog.txt"
Remove-Item $log -ErrorAction SilentlyContinue
Start-Process -FilePath (Join-Path $Pcsx2Dir "pcsx2-qt.exe") -ArgumentList "-elf `"$elf`"" -WorkingDirectory $Pcsx2Dir
Start-Sleep -Seconds $Seconds
Get-Content $log | Select-String -Pattern $Pattern | Select-Object -First 400
