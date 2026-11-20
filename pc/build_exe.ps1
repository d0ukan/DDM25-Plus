# Build the DDM32 Plus programmer as a Windows program that needs no Python on the PC (PyInstaller, pc\bundle.py).
#   powershell -ExecutionPolicy Bypass -File pc\build_exe.ps1             one-folder: pc\dist\DDM32Plus\DDM32Plus.exe
#   powershell -ExecutionPolicy Bypass -File pc\build_exe.ps1 -Paketle    + zip: pc\dist\DDM32Plus-<version>.zip
#   powershell -ExecutionPolicy Bypass -File pc\build_exe.ps1 -OneFile    one file: pc\dist\DDM32Plus.exe
# The program carries the radio tools it drives (pc\radiotools\*.py) and the pinned RadioFoundry files they load
# (hash-checked at build and at run time); it runs them as DDM32Plus.exe --run-tool. The RadioFoundry tree is taken
# from DDM32_UPSTREAM or research\upstream\radiofoundry of the repository (python pc\fetch_upstream.py).
# User data (backups, releases, settings) lives in %LOCALAPPDATA%\DDM32Plus, never in the program folder.
# -Paketle scans the folder for private data (backups, packages, RadioID/TRAC lists, stock firmware, codec bytes,
# keys) and refuses to zip if anything is found. Outputs (pc\build\, pc\dist\) stay out of git.
param([switch]$OneFile, [switch]$Paketle)
$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot
python bundle.py build $(if ($OneFile) { '--onefile' })
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
if (-not $OneFile) {
    & (Join-Path $PSScriptRoot 'dist\DDM32Plus\DDM32Plus.exe') --run-tool _selfcheck | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "Self-check of the built program failed ($LASTEXITCODE)" }
}
if ($Paketle) {
    if ($OneFile) { throw 'Paketle needs the one-folder build' }
    python bundle.py package
    if ($LASTEXITCODE -ne 0) { throw "Paketle failed ($LASTEXITCODE)" }
}
Write-Host "Done: $(Join-Path $PSScriptRoot 'dist')"
