param([string]$HostCompiler = 'gcc')
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$taskPreviousCompiler = $env:POORDS4_HOST_CC
$taskPreviousSource = $env:POORDS4_TEST_ROOT
Push-Location -LiteralPath $taskRepo
try {
    $env:POORDS4_HOST_CC = $HostCompiler
    $env:POORDS4_TEST_ROOT = $taskRepo
    & python -m unittest discover -s tests -p 'test_*.py' -v
    if ($LASTEXITCODE -ne 0) { throw 'Controller contract tests failed.' }
} finally {
    $env:POORDS4_HOST_CC = $taskPreviousCompiler
    $env:POORDS4_TEST_ROOT = $taskPreviousSource
    Pop-Location
}
