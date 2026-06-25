<#
.SYNOPSIS
    Re-initialize or repair git submodules for ccev.

.DESCRIPTION
    Removes deps/ and re-runs git submodule update --init.
    Edit .gitmodules FIRST if you forked to a different git server.

.PARAMETER Force
    Skip confirmation prompt.

.EXAMPLE
    .\scripts\init-deps.ps1
    .\scripts\init-deps.ps1 -Force
#>

param([switch]$Force)

# -- check prerequisites --
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Error "[init-deps] git not found"
    exit 1
}

if (-not (Test-Path ".gitmodules")) {
    Write-Error "[init-deps] no .gitmodules found -- run from ccev project root"
    exit 1
}

# -- prompt --
if (-not $Force) {
    Write-Host "============================================"
    Write-Host "  ccev -- deps re-initialization"
    Write-Host ""
    Write-Host "  This will DELETE deps/ and re-clone all"
    Write-Host "  submodules from the URLs in .gitmodules."
    Write-Host ""
    Write-Host "  If you forked the deps to your own git"
    Write-Host "  server, edit .gitmodules FIRST, then"
    Write-Host "  re-run this script."
    Write-Host "============================================"
    $reply = Read-Host "Proceed? [y/N]"
    if ($reply -notmatch '^y(es)?$') {
        Write-Host "[init-deps] cancelled"
        exit 0
    }
}

# -- clean --
Write-Host "[init-deps] removing deps/ ..."
@('epoll', 'ccalg', 'ccsocket') | ForEach-Object {
    $path = "deps\$_"
    if (Test-Path $path) {
        Remove-Item -Recurse -Force $path
    }
}

# -- re-init --
Write-Host "[init-deps] initializing submodules ..."
git submodule update --init --recursive --force
if ($LASTEXITCODE -ne 0) {
    Write-Error "[init-deps] git submodule update failed"
    exit 1
}

# -- verify --
$missing = @()
@{
    epoll    = "deps\epoll\README.md"
    ccalg    = "deps\ccalg\README.md"
    ccsocket = "deps\ccsocket\README.md"
}.GetEnumerator() | ForEach-Object {
    if (-not (Test-Path $_.Value)) {
        $missing += $_.Key
    }
}

if ($missing.Count -gt 0) {
    Write-Error "[init-deps] submodule(s) missing: $($missing -join ' ')"
    Write-Error "[init-deps]   Check your git server URLs in .gitmodules"
    Write-Error "[init-deps]   or restore connectivity and re-run."
    exit 1
}

Write-Host "[init-deps] OK -- all submodules ready"
exit 0
