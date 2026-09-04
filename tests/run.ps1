$ErrorActionPreference = 'Stop'

$cliRoot = Split-Path -Parent $PSScriptRoot
$compilerCandidates = @(
    (Join-Path (Split-Path -Parent $cliRoot) 'mincc\petcc64.exe'),
    (Join-Path (Split-Path -Parent (Split-Path -Parent $cliRoot)) 'mincc\petcc64.exe')
)
$compiler = $compilerCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$testExe = Join-Path $PSScriptRoot 'usb-eject-tests.exe'
$diagnoseExe = Join-Path $PSScriptRoot 'diagnose-smoke.exe'

if ($null -eq $compiler) {
    throw "mincc compiler not found. Expected ..\mincc\petcc64.exe"
}

& $compiler -std -Wall -Werror -peconsole `
    -I (Join-Path $cliRoot 'src') `
    -o $testExe `
    (Join-Path $PSScriptRoot 'test_main.c') `
    (Join-Path $cliRoot 'src\text.c') `
    (Join-Path $cliRoot 'src\cli.c') `
    (Join-Path $cliRoot 'src\inventory.c') `
    (Join-Path $cliRoot 'src\target.c') `
    (Join-Path $cliRoot 'src\eject.c') `
    (Join-Path $cliRoot 'src\diagnose.c') `
    (Join-Path $cliRoot 'src\action.c') `
    -lkernel32 -lsetupapi -lcfgmgr32 -ladvapi32 -luser32

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $testExe
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $compiler -std -Wall -Werror -peconsole `
    -I (Join-Path $cliRoot 'src') `
    -o $diagnoseExe `
    (Join-Path $PSScriptRoot 'diagnose_smoke.c') `
    (Join-Path $cliRoot 'src\text.c') `
    (Join-Path $cliRoot 'src\diagnose.c') `
    -lkernel32 -ladvapi32

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Push-Location -LiteralPath $cliRoot
try {
    & $diagnoseExe
    $diagnoseExitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}
exit $diagnoseExitCode
