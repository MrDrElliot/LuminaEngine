using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;

namespace LuminaSharp;

/// Collectible load context for one generation of user scripts; only compiled script assemblies live here (so Unload reclaims the generation), referenced framework/engine assemblies defer to Default. Cross-unit references resolve in-ALC.
internal sealed class ScriptLoadContext : AssemblyLoadContext
{
    // hostfxr loads LuminaSharp into its own context (NOT Default), so a script referencing EntityScript must be pointed at that exact assembly.
    private static readonly Assembly EngineAssembly = typeof(Host).Assembly;
    private static readonly string EngineAssemblyName = EngineAssembly.GetName().Name!;

    // Script units in THIS generation, keyed by simple name; lets a dependent unit resolve a sibling dependency to the in-ALC assembly.
    private readonly Dictionary<string, Assembly> ScriptAssemblies = new(StringComparer.OrdinalIgnoreCase);

    public ScriptLoadContext(string Name) : base(Name, isCollectible: true)
    {
    }

    /// Loads one unit's PE image into this context and registers it under UnitName so siblings resolve here; image is copied out of the caller's buffer.
    public Assembly LoadScriptAssembly(string UnitName, FScriptImage Image)
    {
        var PeStream = new MemoryStream(Image.Pe);
        Assembly Loaded;
        // Handing the symbols over here is the only thing that gets line numbers into a script stack trace.
        if (Image.Pdb != null)
        {
            Loaded = LoadFromStream(PeStream, new MemoryStream(Image.Pdb));
        }
        else
        {
            Loaded = LoadFromStream(PeStream);
        }
        ScriptAssemblies[UnitName] = Loaded;
        return Loaded;
    }

    protected override Assembly? Load(AssemblyName Name)
    {
        if (Name.Name == EngineAssemblyName)
        {
            return EngineAssembly;
        }
        if (Name.Name is { } Simple && ScriptAssemblies.TryGetValue(Simple, out Assembly? Sibling))
        {
            return Sibling;
        }
        return null;
    }
}
