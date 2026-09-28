$ErrorActionPreference = "Stop"

$ProjectDir = Split-Path -Parent $PSScriptRoot
$Output = Join-Path $ProjectDir "build\pocket_pet.exe"

if (-not (Get-Command bun -ErrorAction SilentlyContinue)) {
    throw "bun is required to build the Windows executable"
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null
bun build --compile `
    --target=bun-windows-x64 `
    --outfile=$Output `
    (Join-Path $ProjectDir "tools\pet_server.ts")

Write-Output "Windows executable: $Output"
