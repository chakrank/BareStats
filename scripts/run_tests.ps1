<#
.SYNOPSIS
    BareStats Windows Test Runner Wrapper
.DESCRIPTION
    Automates test execution on Windows, configuring the MSVC x64 developer environment
    and running scripts/run_tests.py.
#>

$ErrorActionPreference = "Stop"

# Terminate any lingering test or overlay instances to avoid file lock issues
Get-Process -Name "BareStats*" -ErrorAction SilentlyContinue | Stop-Process -Force

$RepoRoot = Split-Path -Parent $PSScriptRoot
$PythonCmd = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $PythonCmd) {
    $PythonCmd = (Get-Command py -ErrorAction SilentlyContinue).Source
}

if (-not $PythonCmd) {
    Write-Error "Python 3 is required to run the automated test suite."
    exit 1
}

& $PythonCmd "$RepoRoot\scripts\run_tests.py" @args
exit $LASTEXITCODE
