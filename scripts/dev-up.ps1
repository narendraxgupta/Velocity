<#
.SYNOPSIS
    Velocity — Windows / PowerShell wrapper around the Makefile workflow.

.DESCRIPTION
    Equivalent to the top-level Makefile, but native PowerShell for
    developers on Windows who do not have WSL2 or Git Bash. All commands
    eventually shell out to `docker compose`, which is available wherever
    Docker Desktop is installed.

.PARAMETER Action
    One of: bootstrap, up, up-apps, down, nuke, logs, ps, build,
            proto, fmt, lint, test, sample-submit, bench, clean.

.EXAMPLE
    .\scripts\dev-up.ps1 up
    .\scripts\dev-up.ps1 logs
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet(
        'bootstrap', 'up', 'up-apps', 'down', 'nuke',
        'logs', 'ps', 'build', 'proto',
        'fmt', 'lint', 'test',
        'sample-submit', 'bench', 'clean'
    )]
    [string]$Action
)

$ErrorActionPreference = 'Stop'

# Always run from the repository root, regardless of where the script is invoked.
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $repoRoot

function Write-Heading {
    param([string]$Msg)
    Write-Host ""
    Write-Host "→ $Msg" -ForegroundColor Cyan
}

function Invoke-Compose {
    param([string[]]$ComposeArgs)
    & docker compose @ComposeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose failed with exit code $LASTEXITCODE"
    }
}

switch ($Action) {
    'bootstrap' {
        Write-Heading "Pulling base images"
        Invoke-Compose @('pull', '--ignore-pull-failures')
        Write-Heading "Generating protobuf stubs"
        & docker run --rm -v "${repoRoot}/proto:/workspace" -w /workspace bufbuild/buf:1.45.0 generate
        if ($LASTEXITCODE -ne 0) { throw "buf generate failed" }
        Write-Host "✓ bootstrap complete" -ForegroundColor Green
    }

    'up' {
        Invoke-Compose @('--profile', 'default', 'up', '-d')
        Write-Host ""
        Write-Host "✓ infra ready" -ForegroundColor Green
        Write-Host "  Redpanda Console : http://localhost:8085"
        Write-Host "  QuestDB Console  : http://localhost:9000"
        Write-Host "  MinIO Console    : http://localhost:9101"
        Write-Host "  Grafana          : http://localhost:3001"
        Write-Host "  Prometheus       : http://localhost:9090"
    }

    'up-apps' {
        Invoke-Compose @('--profile', 'default', '--profile', 'apps', 'up', '-d')
        Write-Host ""
        Write-Host "✓ platform running" -ForegroundColor Green
        Write-Host "  Frontend         : http://localhost:3000"
        Write-Host "  API Gateway      : http://localhost:8080"
        Write-Host "  Leaderboard WS   : ws://localhost:8090/v1/leaderboard"
    }

    'down'  { Invoke-Compose @('down') }
    'nuke'  { Invoke-Compose @('down', '-v') }
    'logs'  { Invoke-Compose @('logs', '-f', '--tail=100') }
    'ps'    { Invoke-Compose @('ps') }

    'build' { Invoke-Compose @('--profile', 'apps', 'build') }

    'proto' {
        & docker run --rm -v "${repoRoot}/proto:/workspace" -w /workspace bufbuild/buf:1.45.0 generate
        if ($LASTEXITCODE -ne 0) { throw "buf generate failed" }
    }

    'fmt'   { Write-Host "fmt: run inside a Linux build container (see Makefile)" -ForegroundColor Yellow }
    'lint'  { Write-Host "lint: run inside a Linux build container (see Makefile)" -ForegroundColor Yellow }

    'test'  {
        & docker run --rm -v "${repoRoot}:/app" -w /app velocity/cpp-base:builder `
            bash -c "ctest --preset conan-release --output-on-failure"
    }

    'sample-submit' {
        # Runs the same end-to-end smoke flow as `make sample-submit`.
        # Requires the stack to be running (`up-apps`).
        & bash "${repoRoot}/scripts/e2e-smoke.sh"
        if ($LASTEXITCODE -ne 0) { throw "sample-submit failed" }
    }

    'bench' {
        if (-not $env:SUBMISSION_ID) {
            throw "set `$env:SUBMISSION_ID first (see 'dev-up.ps1 sample-submit')"
        }
        $body = @{ submission_id = $env:SUBMISSION_ID; profile = 'baseline' } | ConvertTo-Json -Compress
        Invoke-RestMethod -Method Post -Uri "http://localhost:8080/v1/benchmarks" `
            -ContentType 'application/json' -Body $body
    }

    'clean' {
        Get-ChildItem $repoRoot -Directory `
            | Where-Object Name -in 'build', 'build-Debug', 'build-Release', 'cmake-build-debug', 'cmake-build-release' `
            | Remove-Item -Recurse -Force
        $next = Join-Path $repoRoot 'frontend\.next'
        if (Test-Path $next) { Remove-Item $next -Recurse -Force }
        Write-Host "✓ clean" -ForegroundColor Green
    }
}
