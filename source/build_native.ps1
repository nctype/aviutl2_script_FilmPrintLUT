param(
    [Parameter(Mandatory=$true)][string]$Zig,
    [Parameter(Mandatory=$true)][string]$SdkDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$zigExecutable = (Resolve-Path -LiteralPath $Zig).Path
$sdkInclude = (Resolve-Path -LiteralPath $SdkDirectory).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$binaryDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $binaryDirectory 'zig-cache-global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $binaryDirectory 'zig-cache-local'
& $zigExecutable c++ '-std=c++17' -O2 -shared (Join-Path $PSScriptRoot 'FilmPrintLUTCube.cpp') -I $sdkInclude -o (Join-Path $binaryDirectory 'FilmPrintLUTCube.mod2')
if ($LASTEXITCODE -ne 0) { throw "Native compilation failed: $LASTEXITCODE" }
# Copy only FilmPrintLUTCube.mod2 into Script/FilmPrintLUT for distribution.
