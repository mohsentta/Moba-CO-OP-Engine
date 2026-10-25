# Finishes the repository cleanup.
#
# Why this needs elevation: the leftover directories were created by a process
# running as Administrator, so their files are owned by BUILTIN\Administrators.
# A normal (non-elevated) shell has no WRITE_DAC on them, so it can neither
# delete nor rename them -- and Windows offers no non-elevated way around that.
#
# What it does:
#   1. takes ownership of the tree and grants the signed-in user full control
#      (idempotent; contents are not modified, only permissions)
#   2. deletes the reverse-engineering leftovers and the old repository history
#   3. leaves the new project untouched: README.md, CMakeLists.txt, docs/,
#      include/, src/ (except the old decompiled .c files), tests/, reference/
#
# Run in an Elevated PowerShell (Run as Administrator), from anywhere:
#   powershell -NoProfile -ExecutionPolicy Bypass -File "H:\projects\LOL\scripts\finish-cleanup.ps1"

$ErrorActionPreference = 'Stop'
$root = 'H:\projects\LOL'

if (-not (Test-Path -LiteralPath $root)) { throw "not found: $root" }

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this in an elevated PowerShell (Run as Administrator).'
}

Write-Host '== taking ownership and granting full control ==' -ForegroundColor Cyan
& takeown.exe /F $root /R /D Y 2>&1 | Out-Null
& icacls.exe $root /grant "$($env:USERNAME):(OI)(CI)F" /T /C /Q 2>&1 | Out-Null

Write-Host '== removing decompiled code and old research trees ==' -ForegroundColor Cyan
$trees = @('src_organized', 'old', 'wad_data', 'tools', 'player', 'replay player 2', 'ui_dds', '.git')
foreach ($t in $trees) {
    $p = Join-Path $root $t
    if (Test-Path -LiteralPath $p) {
        Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host ("  {0,-20} {1}" -f $t, $(if (Test-Path -LiteralPath $p) { 'STILL PRESENT' } else { 'removed' }))
    }
}

Write-Host '== removing decompiled C from src/ ==' -ForegroundColor Cyan
Get-ChildItem -LiteralPath (Join-Path $root 'src') -Filter '*.c' -File -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $root 'src\INDEX.md') -Force -ErrorAction SilentlyContinue

Write-Host '== removing loose leftovers at the root ==' -ForegroundColor Cyan
$files = @(
    'function_names.json', 'helper_codes.json', 'modules.tsv', 'renames.tsv',
    'map11_paths.json', 'map11_2048.png', 'map_2048.png', 'sheet.png',
    '_cnt.pkl', '_replay_callers.pkl', '_scan.py'
)
foreach ($f in $files) {
    $p = Join-Path $root $f
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue }
}

Write-Host '== removing stray temp dirs left by blocked writes ==' -ForegroundColor Cyan
Get-ChildItem -LiteralPath $root -Force -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like '..gitignore*' -or $_.Name -like '*.tmpdir' } |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }

Write-Host '== installing the project .gitignore ==' -ForegroundColor Cyan
$gitignore = @'
# Builds
build/
out/
cmake-build-*/
.vs/
CMakeUserPresets.json

# Local, never committed (replays, toolchain, old git bundle)
_local/

# Game assets and derived data must never enter the repository
assets/
*.rofl
*.wad
*.wad.client
*.skn
*.skl
*.anm
*.dds
*.bin

# Legacy leftovers, if any survive
src_organized/
old/
wad_data/
tools/
player/
replay player 2/
ui_dds/
*.pkl
*.exe
'@
Set-Content -LiteralPath (Join-Path $root '.gitignore') -Value $gitignore -Encoding utf8

# Everything this script just created (the .gitignore above) would otherwise be
# owned by the elevated token, which is exactly the problem we are fixing.
# Hand the whole tree back to the signed-in user so later edits need no elevation.
Write-Host '== handing ownership back to your account ==' -ForegroundColor Cyan
& takeown.exe /F $root /R /D Y 2>&1 | Out-Null
& icacls.exe $root /grant "$($env:USERNAME):(OI)(CI)F" /T /C /Q 2>&1 | Out-Null

Write-Host ''
Write-Host '== result ==' -ForegroundColor Green
Get-ChildItem -LiteralPath $root -Force | Select-Object Mode, Name |
    Format-Table -AutoSize | Out-String -Width 120 | Write-Host

$left = Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue |
    Measure-Object Length -Sum
Write-Host ("remaining: {0} files, {1:N1} MiB" -f $left.Count, ($left.Sum / 1MB))
Write-Host ''
Write-Host 'Next: git init, then rename/move nothing else -- the project layout is already final.'
