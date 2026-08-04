# Uploads PDBs and binaries to BugSplat so crash dumps resolve to source lines.
#
# Run this for any build you hand to someone else. It is deliberately NOT part of the normal build:
# a full symbol set is several hundred MB, and pushing that on every incremental compile would be
# both slow and pointless.
#
# The version string must match what the running engine reports, or the service holds symbols it
# will never match to a dump. WindowsCrashReporter.cpp builds it as LUMINA_VERSION-<Configuration>,
# so this reads the same header rather than taking the version on trust.
#
#   $env:SYMBOL_UPLOAD_CLIENT_ID     = "..."
#   $env:SYMBOL_UPLOAD_CLIENT_SECRET = "..."
#   .\Engine\Build\UploadSymbols.ps1 -Configuration Shipping

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Development", "Shipping")]
    [string] $Configuration = "Development",

    [string] $Database = $env:BUGSPLAT_DATABASE,

    [string] $Application = "Lumina",

    [string] $BinariesDirectory,

    # Escape hatch for uploading against a version this working tree no longer matches, e.g.
    # symbols for a build that shipped from a commit you have since moved off.
    [string] $VersionOverride
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

if ([string]::IsNullOrWhiteSpace($Database))
{
    throw "No BugSplat database. Pass -Database or set BUGSPLAT_DATABASE."
}

$HasPassword = -not [string]::IsNullOrWhiteSpace($env:SYMBOL_UPLOAD_PASSWORD)
$HasSecret = -not [string]::IsNullOrWhiteSpace($env:SYMBOL_UPLOAD_CLIENT_SECRET)
if (-not $HasPassword -and -not $HasSecret)
{
    throw "No credentials. Set SYMBOL_UPLOAD_USER + SYMBOL_UPLOAD_PASSWORD, or SYMBOL_UPLOAD_CLIENT_ID + SYMBOL_UPLOAD_CLIENT_SECRET."
}

# Single source of truth for the version, same header the runtime compiles against.
$VersionHeader = Join-Path $RepoRoot "Engine\Source\Runtime\Source\Lumina.h"
$VersionMatch = Select-String -Path $VersionHeader -Pattern '#define\s+LUMINA_VERSION\s+"([^"]+)"'
if ($null -eq $VersionMatch)
{
    throw "Could not read LUMINA_VERSION from $VersionHeader."
}

$Version = "$($VersionMatch.Matches[0].Groups[1].Value)-$Configuration"

# Mirrors WindowsCrashReporter.cpp ReadGitCommit exactly: HEAD, resolve the ref, first 8 chars.
# The runtime appends the commit so reports from source builds are attributable, and BugSplat pairs
# symbols to crashes by version string. Omit it here and the symbols land under a version no crash
# will ever report, which uploads cleanly and resolves nothing.
function Get-GitCommitSuffix
{
    $HeadPath = Join-Path $RepoRoot ".git\HEAD"
    if (-not (Test-Path $HeadPath))
    {
        return ""
    }

    $Head = (Get-Content $HeadPath -Raw).Trim()

    if ($Head.StartsWith("ref: "))
    {
        $RefName = $Head.Substring(5).Trim()
        $RefPath = Join-Path $RepoRoot ".git\$($RefName -replace '/', '\')"
        if (-not (Test-Path $RefPath))
        {
            # Packed ref, same case the runtime gives up on. Both sides then agree on no suffix.
            return ""
        }
        $Head = (Get-Content $RefPath -Raw).Trim()
    }

    if ($Head.Length -lt 8)
    {
        return ""
    }

    return $Head.Substring(0, 8)
}

if ([string]::IsNullOrWhiteSpace($VersionOverride))
{
    $Commit = Get-GitCommitSuffix
    if (-not [string]::IsNullOrWhiteSpace($Commit))
    {
        $Version = "$Version-$Commit"
    }
}
else
{
    $Version = $VersionOverride
}

if ([string]::IsNullOrWhiteSpace($BinariesDirectory))
{
    $BinariesDirectory = Join-Path $RepoRoot "Binaries\Windows64"
}

if (-not (Test-Path $BinariesDirectory))
{
    throw "Binaries directory not found: $BinariesDirectory"
}

$Tool = (Get-Command "symbol-upload" -ErrorAction SilentlyContinue)
if ($null -eq $Tool)
{
    $Tool = (Get-Command "symbol-upload-windows.exe" -ErrorAction SilentlyContinue)
}
if ($null -eq $Tool)
{
    throw "symbol-upload not found on PATH. Install with 'npm i -g @bugsplat/symbol-upload' or download symbol-upload-windows.exe."
}

$SizeBytes = (Get-ChildItem $BinariesDirectory -Recurse -Include *.pdb | Measure-Object -Property Length -Sum).Sum
Write-Host "Uploading $Application $Version from $BinariesDirectory ($([math]::Round($SizeBytes / 1MB)) MB of PDBs)"

# Printed loudly because a mismatch here is silent: the upload succeeds, the crashes arrive, and
# nothing resolves. This string has to be character-for-character what the editor logs at startup
# as "Crash reporting initialized (BugSplat, version ...)".
Write-Host "  Version must match the running engine exactly -- compare against the startup log line." -ForegroundColor Yellow

# BugSplat rejects any single symbol file over 4GB. A monolithic Shipping link is the one that can
# get there, so it is worth naming the offender rather than letting the upload fail late.
$TooLarge = Get-ChildItem $BinariesDirectory -Recurse -Include *.pdb | Where-Object { $_.Length -ge 4GB }
foreach ($File in $TooLarge)
{
    Write-Warning "$($File.Name) is $([math]::Round($File.Length / 1GB, 1)) GB and will be rejected (4GB limit)."
}

& $Tool.Source `
    -b $Database `
    -a $Application `
    -v $Version `
    -d $BinariesDirectory `
    -f "**/*.+(exe|dll|pdb)"

if ($LASTEXITCODE -ne 0)
{
    throw "symbol-upload failed with exit code $LASTEXITCODE."
}

Write-Host "Symbols uploaded for $Application $Version."
