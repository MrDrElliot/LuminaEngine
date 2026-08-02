#Requires -Version 5.1
<#
    Lumina Engine - Prerequisite Validator

    Verifies a machine can BUILD the engine before LuminaBuildTool runs, so a
    fresh clone fails fast with actionable guidance instead of a cryptic
    NETSDK / linker error deep into the build.

    Checks (hard = blocks setup, soft = warning only):
      [hard] Windows curl.exe + tar.exe   (External dependency download)
      [hard] .NET 10 SDK                   (compiles LuminaSharp + game scripts)
      [hard] Visual Studio >= 18.0 (2026)  (only VS 18.0+ MSBuild can target net10.0)
      [soft] Vulkan SDK (VULKAN_SDK)       (validation layers only; headers are bundled)

    Exit codes: 0 = all hard checks passed, 1 = one or more hard checks failed.
    Pass -NonBlocking to downgrade hard failures to warnings (exit 0 regardless).
#>

[CmdletBinding()]
param
(
    [int]    $RequiredDotNetMajor = 10,
    [version]$RequiredVSVersion   = '18.0',
    [switch] $NonBlocking
)

$ErrorActionPreference = 'Stop'

# These mirror LuminaSharp.csproj <TargetFramework> and the VS-version gate that
# produces NETSDK1209. Bump them together if the engine retargets.
$DotNetSdkUrl = "https://dotnet.microsoft.com/download/dotnet/$RequiredDotNetMajor.0"
$VisualStudioUrl = 'https://visualstudio.microsoft.com/downloads/'
$VulkanSdkUrl = 'https://vulkan.lunarg.com/sdk/home'

$script:HardFailures = New-Object System.Collections.Generic.List[string]
$script:SoftWarnings = New-Object System.Collections.Generic.List[string]

function Write-Status
{
    param
    (
        [ValidateSet('PASS', 'FAIL', 'WARN', 'INFO')] [string]$Kind,
        [string]$Title,
        [string]$Detail
    )

    $color = switch ($Kind)
    {
        'PASS' { 'Green' }
        'FAIL' { 'Red' }
        'WARN' { 'Yellow' }
        default { 'Gray' }
    }

    Write-Host ('  [{0}] ' -f $Kind) -ForegroundColor $color -NoNewline
    Write-Host $Title
    if ($Detail)
    {
        Write-Host ('        {0}' -f $Detail) -ForegroundColor DarkGray
    }
}

function Add-HardFailure
{
    param ([string]$Title, [string]$Remedy)
    Write-Status -Kind FAIL -Title $Title -Detail $Remedy
    $script:HardFailures.Add($Title)
}

function Add-SoftWarning
{
    param ([string]$Title, [string]$Remedy)
    Write-Status -Kind WARN -Title $Title -Detail $Remedy
    $script:SoftWarnings.Add($Title)
}


# --- 1. Windows tooling (curl.exe + tar.exe) ---------------------------------
function Test-WindowsTooling
{
    foreach ($tool in 'curl.exe', 'tar.exe')
    {
        $cmd = Get-Command $tool -ErrorAction SilentlyContinue
        if ($cmd)
        {
            Write-Status -Kind PASS -Title "$tool found" -Detail $cmd.Source
        }
        else
        {
            Add-HardFailure "$tool not on PATH" `
                'Requires Windows 10 1803+ (both ship in-box). Update Windows or add them to PATH.'
        }
    }
}


# --- 2. .NET SDK -------------------------------------------------------------
function Test-DotNetSdk
{
    $dotnet = Get-Command dotnet.exe -ErrorAction SilentlyContinue
    if (-not $dotnet)
    {
        Add-HardFailure '.NET SDK not found (dotnet.exe missing)' `
            "Install the .NET $RequiredDotNetMajor SDK (x64): $DotNetSdkUrl"
        return
    }

    $sdks = @()
    try
    {
        $sdks = & dotnet.exe --list-sdks 2>$null
    }
    catch
    {
        # fall through to the empty-list failure below
    }

    $majors = @()
    foreach ($line in $sdks)
    {
        if ($line -match '^(\d+)\.\d+\.\d+')
        {
            $majors += [int]$Matches[1]
        }
    }

    if ($majors -contains $RequiredDotNetMajor)
    {
        $match = ($sdks | Where-Object { $_ -match "^$RequiredDotNetMajor\." } | Select-Object -First 1)
        Write-Status -Kind PASS -Title ".NET $RequiredDotNetMajor SDK installed" -Detail ($match -replace '\s*\[.*$', '').Trim()
    }
    else
    {
        $have = if ($majors.Count) { 'found: ' + (($majors | Sort-Object -Unique) -join ', ') } else { 'no SDKs found' }
        Add-HardFailure ".NET $RequiredDotNetMajor SDK not installed ($have)" `
            "Install the .NET $RequiredDotNetMajor SDK (x64): $DotNetSdkUrl"
    }
}


# --- 3. Visual Studio (>= 18.0 for net10.0 / NETSDK1209) ---------------------
function Test-VisualStudio
{
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere))
    {
        Add-HardFailure 'Visual Studio not detected (vswhere.exe missing)' `
            "Install Visual Studio $RequiredVSVersion+ (2026) with the 'Desktop development with C++' and '.NET desktop' workloads: $VisualStudioUrl"
        return
    }

    # -prerelease + -all so Insider/Preview channels and every install are considered.
    $versions = @()
    try
    {
        $versions = & $vswhere -prerelease -all -products * -property installationVersion 2>$null |
            Where-Object { $_ -match '^\d' }
    }
    catch
    {
    }

    if (-not $versions -or $versions.Count -eq 0)
    {
        Add-HardFailure 'No Visual Studio installation found' `
            "Install Visual Studio $RequiredVSVersion+ (2026): $VisualStudioUrl"
        return
    }

    $best = ($versions | ForEach-Object { [version]($_ -replace '[^0-9.].*$', '') } |
        Sort-Object -Descending | Select-Object -First 1)

    if ($best -ge $RequiredVSVersion)
    {
        Write-Status -Kind PASS -Title "Visual Studio $best detected" -Detail "supports net10.0 (>= $RequiredVSVersion)"
    }
    else
    {
        Add-HardFailure "Visual Studio $best is too old for net10.0" `
            ("Only VS $RequiredVSVersion+ (2026) MSBuild can target net10.0 -> error NETSDK1209. " +
             "Installing the standalone .NET SDK does NOT help; VS uses its own bundled MSBuild. " +
             "Install VS $RequiredVSVersion+: $VisualStudioUrl")
    }
}


# --- 4. Vulkan SDK (soft: validation layers only) ----------------------------
function Test-VulkanSdk
{
    $sdk = $env:VULKAN_SDK
    if ($sdk -and (Test-Path $sdk))
    {
        Write-Status -Kind PASS -Title 'Vulkan SDK found' -Detail $sdk
    }
    else
    {
        Add-SoftWarning 'Vulkan SDK (VULKAN_SDK) not set' `
            ("Optional: only needed for Vulkan validation layers in Debug. Engine headers are bundled and " +
             "the loader ships with GPU drivers. Install for debugging: $VulkanSdkUrl")
    }
}


# --- Run ---------------------------------------------------------------------
Write-Host ''
Write-Host '============================================================'
Write-Host ' LUMINA ENGINE - PREREQUISITE CHECK'
Write-Host '============================================================'
Write-Host ''

Test-WindowsTooling
Test-DotNetSdk
Test-VisualStudio
Test-VulkanSdk

Write-Host ''
if ($script:HardFailures.Count -eq 0)
{
    if ($script:SoftWarnings.Count -gt 0)
    {
        Write-Host ("All required prerequisites met ({0} optional warning(s) above)." -f $script:SoftWarnings.Count) -ForegroundColor Green
    }
    else
    {
        Write-Host 'All prerequisites met.' -ForegroundColor Green
    }
    Write-Host ''
    exit 0
}

Write-Host ("{0} required prerequisite(s) missing:" -f $script:HardFailures.Count) -ForegroundColor Red
foreach ($f in $script:HardFailures)
{
    Write-Host ("  - {0}" -f $f) -ForegroundColor Red
}
Write-Host ''

if ($NonBlocking)
{
    Write-Host 'Continuing anyway (-NonBlocking). The build will likely fail until these are resolved.' -ForegroundColor Yellow
    Write-Host ''
    exit 0
}

exit 1
