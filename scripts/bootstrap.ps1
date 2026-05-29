<#
.SYNOPSIS
    Velocity - first-time developer setup.

.DESCRIPTION
    Validates that the developer's machine has the required tooling and
    pulls down the heavy base images so the first `up` is fast. Idempotent.

    Required:
      * Docker Desktop (with the Linux engine running)
      * git
      * 16+ GB RAM allocated to Docker
      * 30+ GB free disk

    Optional:
      * Node 20+ (for local frontend dev outside Docker)
      * Go 1.22+ (for local Go dev outside Docker)

.EXAMPLE
    .\scripts\bootstrap.ps1
#>

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $repoRoot

function Test-Tool {
    param([string]$Command, [string]$Hint = "")
    $found = Get-Command $Command -ErrorAction SilentlyContinue
    if (-not $found) {
        $msg = "[X] Missing required tool: $Command"
        if ($Hint) { $msg += "  ($Hint)" }
        Write-Host $msg -ForegroundColor Red
        return $false
    }
    Write-Host ("[OK] {0,-10}  {1}" -f $Command, $found.Source) -ForegroundColor Green
    return $true
}

function Test-Optional {
    param([string]$Command, [string]$Hint = "")
    $found = Get-Command $Command -ErrorAction SilentlyContinue
    if ($found) {
        Write-Host ("[OK] {0,-10}  {1}" -f $Command, $found.Source) -ForegroundColor Green
    } else {
        $msg = "[-]  Optional   $Command not found"
        if ($Hint) { $msg += "  ($Hint)" }
        Write-Host $msg -ForegroundColor DarkYellow
    }
}

Write-Host ""
Write-Host "Velocity - Developer Bootstrap" -ForegroundColor Cyan
Write-Host "==============================" -ForegroundColor Cyan
Write-Host ""

$ok = $true
$ok = (Test-Tool 'docker' 'install Docker Desktop')   -and $ok
$ok = (Test-Tool 'git' 'install Git for Windows')      -and $ok

Test-Optional 'node' 'install Node.js 20 LTS for local frontend dev'
Test-Optional 'go'   'install Go 1.22+ for local Go dev'

if (-not $ok) {
    Write-Host ""
    Write-Host "Bootstrap failed: install the missing required tools and re-run." -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "-> Verifying Docker daemon" -ForegroundColor Cyan
& docker info > $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Docker daemon is not responsive. Start Docker Desktop and re-run." -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Docker daemon responsive" -ForegroundColor Green

Write-Host ""
Write-Host "-> Pre-pulling infrastructure images (this is slow on first run)" -ForegroundColor Cyan
& docker compose pull --ignore-pull-failures
if ($LASTEXITCODE -ne 0) {
    Write-Host "docker compose pull failed" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "-> Generating protobuf stubs" -ForegroundColor Cyan
& docker run --rm -v "${repoRoot}/proto:/workspace" -w /workspace bufbuild/buf:1.45.0 generate
if ($LASTEXITCODE -ne 0) {
    Write-Host "buf generate failed - check proto/ for syntax errors" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "[OK] Bootstrap complete." -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  .\scripts\dev-up.ps1 up        # start the infrastructure"
Write-Host "  .\scripts\dev-up.ps1 up-apps   # also start the application services (needs ``build`` first)"
Write-Host "  http://localhost:3000          # frontend, once apps are up"
Write-Host ""
