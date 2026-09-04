$ErrorActionPreference = 'Stop'

$cliRoot = Split-Path -Parent $PSScriptRoot
$expectedProjectName = 'usb-eject'
$forbiddenProjectNames = @(
    (@('usb-eject', 'cli') -join '-'),
    (@('usb-eject', 'windows', 'x64') -join '-'),
    (@('USB Eject', 'CLI') -join ' '),
    (@('USB Disk Ejector', 'CLI') -join ' ')
)

if ((Split-Path -Leaf $cliRoot) -cne $expectedProjectName) {
    throw "Project folder must be named exactly '$expectedProjectName'"
}

$versionedFiles = & git -C $cliRoot ls-files
if ($LASTEXITCODE -ne 0) {
    throw 'Could not enumerate versioned files for the project-name check'
}
foreach ($relativePath in $versionedFiles) {
    $path = Join-Path $cliRoot $relativePath
    $contents = Get-Content -LiteralPath $path -Raw
    foreach ($forbiddenName in $forbiddenProjectNames) {
        if ($contents -match [regex]::Escape($forbiddenName)) {
            throw "Alternate project name '$forbiddenName' found in $relativePath"
        }
    }
}

$originUrl = & git -C $cliRoot config --get remote.origin.url
if ($LASTEXITCODE -eq 0 -and $originUrl) {
    $originName = [regex]::Match($originUrl.Trim(), '[/\\:]([^/\\:]+?)(?:\.git)?$').Groups[1].Value
    if ($originName -cne $expectedProjectName) {
        throw "Git origin repository must be named exactly '$expectedProjectName'"
    }
}

$compilerCandidates = @(
    (Join-Path (Split-Path -Parent $cliRoot) 'mincc\petcc64.exe'),
    (Join-Path (Split-Path -Parent (Split-Path -Parent $cliRoot)) 'mincc\petcc64.exe')
)
$compiler = $compilerCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$testExe = Join-Path $PSScriptRoot 'usb-eject-tests.exe'
$diagnoseExe = Join-Path $PSScriptRoot 'diagnose-smoke.exe'
$cliExe = Join-Path $PSScriptRoot 'usb-eject-smoke.exe'

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
    (Join-Path $cliRoot 'src\portable.c') `
    (Join-Path $cliRoot 'src\output.c') `
    -lkernel32 -lshell32 -lsetupapi -lcfgmgr32 -ladvapi32 -luser32

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $testExe
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $compiler -std -Wall -Werror -peconsole `
    -I (Join-Path $cliRoot 'src') `
    -o $cliExe `
    (Join-Path $cliRoot 'src\main.c') `
    (Join-Path $cliRoot 'src\text.c') `
    (Join-Path $cliRoot 'src\cli.c') `
    (Join-Path $cliRoot 'src\inventory.c') `
    (Join-Path $cliRoot 'src\target.c') `
    (Join-Path $cliRoot 'src\eject.c') `
    (Join-Path $cliRoot 'src\diagnose.c') `
    (Join-Path $cliRoot 'src\action.c') `
    (Join-Path $cliRoot 'src\output.c') `
    (Join-Path $cliRoot 'src\portable.c') `
    -lkernel32 -lshell32 -lsetupapi -lcfgmgr32 -ladvapi32 -luser32

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

function Invoke-CliSmoke([string]$Arguments) {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $cliExe
    $start.Arguments = $Arguments
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) { throw "Could not start CLI smoke test" }
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    [pscustomobject]@{ ExitCode = $process.ExitCode; Stdout = $stdout; Stderr = $stderr }
}

$version = Invoke-CliSmoke '--version'
if ($version.ExitCode -ne 0 -or $version.Stdout -notmatch '^usb-eject 0\.1\.0' -or $version.Stderr) {
    throw "--version CLI contract failed"
}
$help = Invoke-CliSmoke 'help'
if ($help.ExitCode -ne 0 -or $help.Stdout -notmatch '--no-prompt' -or
    $help.Stdout -notmatch 'Exit codes:' -or $help.Stderr) {
    throw "help CLI contract failed"
}
$ejectHelp = Invoke-CliSmoke 'eject --help'
if ($ejectHelp.ExitCode -ne 0 -or $ejectHelp.Stdout -notmatch 'Forced termination' -or
    $ejectHelp.Stderr) {
    throw "eject help CLI contract failed"
}
$badLetter = Invoke-CliSmoke 'diagnose --letter 12'
if ($badLetter.ExitCode -ne 1 -or $badLetter.Stdout -or
    $badLetter.Stderr -notmatch '--letter expects A through Z') {
    throw "invalid-letter CLI contract failed"
}
$listTsv = Invoke-CliSmoke 'list --format tsv'
if ($listTsv.ExitCode -ne 0 -or
    $listTsv.Stdout -notmatch '^mount_point\tlabel\tvendor\tproduct' -or
    $listTsv.Stderr) {
    throw "list TSV CLI contract failed"
}
$forgedContinuation = Invoke-CliSmoke '--internal-wait-pid 1 instance volume eject --mount E:'
if ($forgedContinuation.ExitCode -ne 1 -or
    $forgedContinuation.Stderr -notmatch 'Invalid internal continuation command') {
    throw "internal-continuation validation failed"
}
$forgedWorker = Invoke-CliSmoke '--internal-handle-worker'
if ($forgedWorker.ExitCode -ne 1) {
    throw "internal-worker validation failed"
}
Write-Output 'CLI smoke tests passed'

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
