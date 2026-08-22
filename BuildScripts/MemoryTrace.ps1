<#
.SYNOPSIS
    Captures an ETW trace that attributes EVERY byte of process memory to a call stack,
    including allocations the in-engine Memory Profiler cannot see (slang.dll, the GPU
    driver, basisu, the CRT heap, rpmalloc's own OS mappings).

.DESCRIPTION
    Two capture modes, because Windows has two independent memory paths:

      VirtualAlloc  Kernel VIRT_ALLOC provider + stacks. Sees every VirtualAlloc/VirtualFree
                    in the process. This is what rpmalloc, the GPU driver, and any large
                    allocator use directly. Low overhead, safe to leave running for minutes.

      Heap          NT-heap provider + stacks. Sees every HeapAlloc/HeapFree, which is what
                    anything in a separate DLL using the CRT does -- slang.dll, the Vulkan
                    driver's internals, Luau, basisu. Must LAUNCH the process (heap tracing
                    is enabled at process creation), high overhead, ETL grows fast.

    Analyze with WPA:
      VirtualAlloc trace -> graph "VirtualAlloc Commit LifeTimes", set the "AllocType" filter
                            to Outstanding, expand the Stack column. Sort by Size.
      Heap trace         -> graph "Heap Outstanding Allocation Size", expand Stack, sort by Size.

.EXAMPLE
    # Attach to a repro you drive by hand:
    .\MemoryTrace.ps1 -Mode VirtualAlloc

.EXAMPLE
    # Launch the editor under heap tracing (catches Slang / driver / CRT):
    .\MemoryTrace.ps1 -Mode Heap -Exe Lumina-Editor-Development.exe
#>
[CmdletBinding()]
param
(
    [ValidateSet('VirtualAlloc', 'Heap')]
    [string] $Mode = 'VirtualAlloc',

    [string] $Exe = 'Lumina-Editor-Development.exe',

    [string] $OutDir = (Join-Path $PSScriptRoot '..\Saved\MemoryTraces')
)

$ErrorActionPreference = 'Stop'

$Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($Identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
{
    throw 'ETW kernel tracing requires an elevated shell. Re-run this from an Administrator PowerShell.'
}

$XPerf = 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe'
if (-not (Test-Path $XPerf))
{
    throw "xperf.exe not found at $XPerf. Install the Windows Performance Toolkit (Windows SDK component)."
}

$BinDir = Resolve-Path (Join-Path $PSScriptRoot '..\Binaries\Windows64')
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
$OutDir = Resolve-Path $OutDir

# Engine PDBs first, then the public symbol server for ntdll / the GPU driver / ucrtbase.
$SymCache = Join-Path $env:LOCALAPPDATA 'SymCache'
if (-not (Test-Path $SymCache)) { New-Item -ItemType Directory -Force -Path $SymCache | Out-Null }
$env:_NT_SYMBOL_PATH = "$BinDir;srv*$SymCache*https://msdl.microsoft.com/download/symbols"
Write-Host "Symbols: $env:_NT_SYMBOL_PATH" -ForegroundColor DarkGray

$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$Etl   = Join-Path $OutDir "$Mode-$Stamp.etl"

if ($Mode -eq 'VirtualAlloc')
{
    Write-Host 'Starting VirtualAlloc trace (all processes; filter to Lumina in WPA)...' -ForegroundColor Cyan
    & $XPerf -on PROC_THREAD+LOADER+VIRT_ALLOC -stackwalk VirtualAlloc+VirtualFree `
             -BufferSize 1024 -MinBuffers 512 -MaxBuffers 1024
    if ($LASTEXITCODE -ne 0) { throw "xperf -on failed ($LASTEXITCODE)" }

    Write-Host ''
    Write-Host '  Trace is RUNNING.' -ForegroundColor Green
    Write-Host '  Start the engine, drive the repro until memory is high, then come back here.'
    Write-Host ''
    Read-Host '  Press Enter to stop and write the trace'

    & $XPerf -d $Etl
}
else
{
    $ExePath = Join-Path $BinDir $Exe
    if (-not (Test-Path $ExePath)) { throw "Executable not found: $ExePath" }

    Write-Host 'Starting kernel + heap trace and launching the process...' -ForegroundColor Cyan
    Write-Host 'Heap tracing is heavy -- keep the repro short and expect a multi-GB ETL.' -ForegroundColor Yellow

    & $XPerf -on PROC_THREAD+LOADER -BufferSize 1024 -MinBuffers 256 -MaxBuffers 512
    if ($LASTEXITCODE -ne 0) { throw "xperf -on failed ($LASTEXITCODE)" }

    $HeapEtl = Join-Path $OutDir "heap-$Stamp.raw.etl"
    & $XPerf -start HeapSession -heap -PidNewProcess "`"$ExePath`"" `
             -stackwalk HeapAlloc+HeapRealloc+HeapFree `
             -BufferSize 1024 -MinBuffers 512 -MaxBuffers 1024 -f $HeapEtl
    if ($LASTEXITCODE -ne 0) { & $XPerf -stop | Out-Null; throw "xperf -start HeapSession failed ($LASTEXITCODE)" }

    Write-Host ''
    Write-Host '  Trace is RUNNING and the engine has been launched.' -ForegroundColor Green
    Write-Host '  Drive the repro, then come back here (leave the engine open).'
    Write-Host ''
    Read-Host '  Press Enter to stop and merge the trace'

    & $XPerf -stop HeapSession -stop -d $Etl
}

if ($LASTEXITCODE -ne 0) { throw "xperf stop/merge failed ($LASTEXITCODE)" }

Write-Host ''
Write-Host "Trace written: $Etl" -ForegroundColor Green
Write-Host 'Opening in WPA...' -ForegroundColor Cyan
Start-Process 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\wpa.exe' -ArgumentList "`"$Etl`""
