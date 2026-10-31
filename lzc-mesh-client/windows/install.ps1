# Installs 懒猫设备接力 (LZC Mesh) for the current user: copies the exe to
# %LOCALAPPDATA%\Programs\LZC Mesh, creates Start menu / autostart shortcuts and
# launches it. No admin rights needed.
$ErrorActionPreference = 'Stop'
$name = [string][char[]]@(0x61D2,0x732B,0x8BBE,0x5907,0x63A5,0x529B) -replace ' ', ''   # 懒猫设备接力 (kept ASCII-safe for Windows PowerShell 5)
$src = Join-Path $PSScriptRoot 'LZC Mesh.exe'
$dir = Join-Path $env:LOCALAPPDATA 'Programs\LZC Mesh'
Get-Process -Name 'LZC Mesh' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Copy-Item -Force $src (Join-Path $dir 'LZC Mesh.exe')
$exe = Join-Path $dir 'LZC Mesh.exe'
$programs = [Environment]::GetFolderPath('Programs')
$startup = [Environment]::GetFolderPath('Startup')
# Shortcuts from v0.1.0
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $programs 'LZC Mesh.lnk'), (Join-Path $startup 'LZC Mesh.lnk')
$shell = New-Object -ComObject WScript.Shell
foreach ($lnk in @((Join-Path $programs "$name.lnk"), (Join-Path $startup "$name.lnk"))) {
  $s = $shell.CreateShortcut($lnk); $s.TargetPath = $exe; $s.WorkingDirectory = $dir; $s.Description = $name; $s.Save()
}
Start-Process $exe
Write-Host "$name installed to $dir"
