# Installs LZC Mesh for the current user: copies the exe to %LOCALAPPDATA%\Programs,
# creates Start menu / autostart shortcuts and launches it. No admin rights needed.
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'LZC Mesh.exe'
$dir = Join-Path $env:LOCALAPPDATA 'Programs\LZC Mesh'
Get-Process -Name 'LZC Mesh' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Copy-Item -Force $src (Join-Path $dir 'LZC Mesh.exe')
$exe = Join-Path $dir 'LZC Mesh.exe'
$shell = New-Object -ComObject WScript.Shell
foreach ($lnk in @((Join-Path ([Environment]::GetFolderPath('Programs')) 'LZC Mesh.lnk'),
                   (Join-Path ([Environment]::GetFolderPath('Startup')) 'LZC Mesh.lnk'))) {
  $s = $shell.CreateShortcut($lnk); $s.TargetPath = $exe; $s.WorkingDirectory = $dir; $s.Save()
}
Start-Process $exe
Write-Host "LZC Mesh installed to $dir"
