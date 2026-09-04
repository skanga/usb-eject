$ErrorActionPreference = 'Stop'

$cliRoot = $PSScriptRoot
$compilerCandidates = @(
    (Join-Path (Split-Path -Parent $cliRoot) 'mincc\petcc64.exe'),
    (Join-Path (Split-Path -Parent (Split-Path -Parent $cliRoot)) 'mincc\petcc64.exe')
)
$compiler = $compilerCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$buildDir = Join-Path $cliRoot 'build'
$output = Join-Path $buildDir 'usb-eject.exe'

if ($null -eq $compiler) {
    throw "mincc compiler not found. Expected ..\mincc\petcc64.exe"
}

New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

$sources = @(
    'main.c',
    'text.c',
    'cli.c',
    'inventory.c',
    'target.c',
    'eject.c',
    'diagnose.c',
    'action.c',
    'output.c'
    'portable.c'
) | ForEach-Object { Join-Path $cliRoot "src\$_" }

& $compiler -std -Wall -Werror -peconsole `
    -I (Join-Path $cliRoot 'src') `
    -o $output `
    @sources `
    -lkernel32 -lshell32 -lsetupapi -lcfgmgr32 -ladvapi32 -luser32

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Output "Built $output"
