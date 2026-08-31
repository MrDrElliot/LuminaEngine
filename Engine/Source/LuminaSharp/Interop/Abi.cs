using System;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>
/// Native -&gt; managed log severity. Mirrors the levels FDotNetHost::Export_Log switches on.
/// </summary>
public enum ELogLevel
{
    Trace = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
}

/// <summary>
/// Table of native function pointers handed to managed at bootstrap. Mirrors
/// Lumina::DotNet::FExporterTable exactly (layout + calling convention).
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct FExporterTable
{
    public delegate* unmanaged[Stdcall]<int, byte*, int, void> Log;
}

/// <summary>
/// Handshake payload passed to <see cref="Host.Bootstrap"/>. Mirrors Lumina::DotNet::FBootstrapArgs.
/// Native->managed entries are no longer handed over as a struct: Bootstrap registers the engine exports into
/// <see cref="ManagedExportRegistry"/> and native resolves each by name (see DotNet::ResolveManagedExport).
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct FBootstrapArgs
{
    public int AbiVersion;
    public FExporterTable* Exports;
    public IntPtr NativeModule;
}

/// <summary>
/// One script source handed from native (already read through the engine VFS). Mirrors
/// Lumina::DotNet::FSourceFile; natural alignment matches the native struct on x64.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct FSourceFile
{
    public byte* Path;
    public int PathLength;
    public byte* Text;
    public int TextLength;
}

/// <summary>
/// One compilation unit handed from native: a plugin (or the game, or the engine library) that owns a
/// set of script sources and depends on a set of sibling units by name. Mirrors
/// Lumina::DotNet::FSourceAssembly. A unit with no sources but a non-empty <see cref="DllPath"/> is a
/// prebuilt managed assembly the host loads as-is instead of compiling. Each unit becomes one assembly
/// in the shared collectible script ALC; <see cref="Deps"/> is a ';'-joined list of the unit names this
/// one references (used to order compilation and wire cross-assembly metadata references).
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct FSourceAssembly
{
    public byte* Name;
    public int NameLength;
    public byte* Deps;          // ';'-joined dependency unit names ("" if none)
    public int DepsLength;
    public FSourceFile* Sources;
    public int SourceCount;
    public byte* DllPath;       // optional prebuilt managed DLL (used when SourceCount == 0); "" otherwise
    public int DllPathLength;
    public byte* References;    // ';'-joined absolute paths to third-party assemblies ("" if none)
    public int ReferencesLength;
}
