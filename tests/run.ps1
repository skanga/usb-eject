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

function Invoke-CliSmoke([string]$Arguments, [string]$Executable = $cliExe) {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.Arguments = $Arguments
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) { throw "Could not start CLI smoke test" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(60000)) {
        $process.Kill()
        throw "CLI test timed out: $Arguments"
    }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    [pscustomobject]@{ ExitCode = $process.ExitCode; Stdout = $stdout; Stderr = $stderr }
}

$version = Invoke-CliSmoke '--version'
if ($version.ExitCode -ne 0 -or $version.Stdout -notmatch '^usb-eject 0\.2\.0' -or $version.Stderr) {
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
if ($listTsv.ExitCode -notin @(0, 9) -or
    $listTsv.Stdout -notmatch '^mount_point\tlabel\tvendor\tproduct' -or
    ($listTsv.ExitCode -eq 0 -and $listTsv.Stderr) -or
    ($listTsv.ExitCode -eq 9 -and $listTsv.Stderr -notmatch 'Discovery incomplete')) {
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

foreach ($arguments in @('eject E: --help', 'diagnose --label=--help --help', 'list --help --bad')) {
    $result = Invoke-CliSmoke $arguments
    if ($result.ExitCode -ne 0 -or $result.Stderr -or $result.Stdout -notmatch 'Usage:') {
        throw "Contextual help failed: $arguments"
    }
}
$missing = Invoke-CliSmoke 'diagnose --label --no-prompt'
if ($missing.ExitCode -ne 1 -or $missing.Stdout -or $missing.Stderr -notmatch "at '--label'.*requires a value") {
    throw 'Missing label must identify the option and the missing value'
}
$unknown = Invoke-CliSmoke 'eject E: --typo'
if ($unknown.ExitCode -ne 1 -or $unknown.Stdout -or $unknown.Stderr -notmatch "at '--typo'") {
    throw 'Unknown option must identify the offending token'
}
$receiptOption = Invoke-CliSmoke 'eject E: --result-file unused.txt'
if ($receiptOption.ExitCode -ne 1 -or $receiptOption.Stderr -notmatch 'only with eject --this') {
    throw 'Result receipt must be restricted to portable ejection'
}

$allModules = @('text', 'cli', 'inventory', 'target', 'eject', 'diagnose', 'action', 'output', 'portable')
function Build-RegressionTest([string]$Name, [string]$Source, [string[]]$Modules) {
    $sources = @((Join-Path $PSScriptRoot $Source))
    $sources += $Modules | ForEach-Object { Join-Path $cliRoot "src\$_.c" }
    & $compiler -std -Wall -Werror -peconsole -I (Join-Path $cliRoot 'src') `
        -o (Join-Path $PSScriptRoot "$Name.exe") @sources `
        -lkernel32 -lshell32 -lsetupapi -lcfgmgr32 -ladvapi32 -luser32
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $Name" }
}
Build-RegressionTest 'flow-tests' 'flow_tests.c' $allModules
Build-RegressionTest 'card-tests' 'card_tests.c' @('target', 'text')
Build-RegressionTest 'portable-tests' 'portable_tests.c' ($allModules | Where-Object { $_ -ne 'portable' })
Build-RegressionTest 'deadline-tests' 'diagnostic_deadline_tests.c' @('target', 'text')
Build-RegressionTest 'inventory-tests' 'inventory_tests.c' @('text')
foreach ($name in @('card-tests', 'portable-tests', 'deadline-tests', 'inventory-tests')) {
    $result = Invoke-CliSmoke '' (Join-Path $PSScriptRoot "$name.exe")
    if ($result.ExitCode -ne 0) { throw "$name failed: $($result.Stdout) $($result.Stderr)" }
}
$flowExe = Join-Path $PSScriptRoot 'flow-tests.exe'
foreach ($case in @('partial', 'quiet', 'multiple', 'remaining', 'inventory', 'force', 'unsafe-scan', 'media-ambiguity', 'redirect')) {
    $result = Invoke-CliSmoke $case $flowExe
    if ($result.ExitCode -ne 0 -or $result.Stdout) {
        throw "Flow regression $case failed: $($result.Stdout) $($result.Stderr)"
    }
}
$targetOutput = Invoke-CliSmoke 'target' $flowExe
if ($targetOutput.ExitCode -ne 0 -or $targetOutput.Stdout -notmatch 'Read-only inspection' -or
    $targetOutput.Stdout -notmatch 'Photos' -or $targetOutput.Stdout -notmatch 'Archive' -or
    $targetOutput.Stdout -match 'will be removed') { throw 'Diagnosis scope message failed' }
$cardOutput = Invoke-CliSmoke 'card-target' $flowExe
if ($cardOutput.ExitCode -ne 0 -or $cardOutput.Stdout -notmatch 'keep the reader' -or
    $cardOutput.Stdout -match 'Archive') { throw 'Card scope must exclude sibling media' }
$grouped = Invoke-CliSmoke 'grouped' $flowExe
if ($grouped.ExitCode -ne 0 -or ([regex]::Matches($grouped.Stdout, 'PID 123')).Count -ne 1 -or
    $grouped.Stdout -match '(?m)^\s+native\s|0x0000' -or $grouped.Stdout -notmatch '4 resource findings') {
    throw 'Default diagnostics must group resources by process'
}
$verbose = Invoke-CliSmoke 'verbose' $flowExe
if ($verbose.ExitCode -ne 0 -or $verbose.Stdout -notmatch 'native' -or
    ([regex]::Matches($verbose.Stdout, 'handle   0x')).Count -ne 4) { throw 'Verbose diagnostics lost details' }
$tsv = Invoke-CliSmoke 'diagnostic-tsv' $flowExe
if ($tsv.ExitCode -ne 0 -or ($tsv.Stdout.Trim() -split "`r?`n").Count -ne 5 -or $tsv.Stderr) {
    throw 'TSV diagnostics must preserve one row per finding'
}
$recovery = Invoke-CliSmoke 'recovery' $flowExe
if ($recovery.ExitCode -ne 0 -or $recovery.Stderr -or
    $recovery.Stdout -notmatch "O''Brien & Photos" -or
    $recovery.Stdout -notmatch '--card --quiet --no-prompt --verbose' -or
    $recovery.Stdout -notmatch '--kill-blocker 123 --yes') { throw 'Quoted recovery command failed' }
Write-Output 'Recovery, card, portable, inventory, output, and deadline regressions passed'

& $compiler -std -Wall -Werror -peconsole `
    -I (Join-Path $cliRoot 'src') `
    -o $diagnoseExe `
    (Join-Path $PSScriptRoot 'diagnose_smoke.c') `
    (Join-Path $cliRoot 'src\target.c') `
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
