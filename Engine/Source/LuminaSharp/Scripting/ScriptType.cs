using System;
using System.Collections.Generic;

namespace LuminaSharp;

/// <summary>Discriminator for a script property's reflected type, driving which native FProperty is minted.</summary>
public enum EScriptKind : byte
{
    Nil = 0,
    Bool,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    String,
    Enum,
    NativeStruct,
    ScriptStruct,
    AssetRef,
    Entity,
    Array,
}

/// <summary>Coarse self-describing kind on the value wire; integers and enums and entity are Int, asset refs String, structs Nested.</summary>
public enum EScriptValueKind : byte
{
    Nil = 0,
    Bool,
    Int,
    Double,
    String,
    Nested,
    Array,
}

/// <summary>One enumerator of a C# enum exposed to the editor.</summary>
public readonly struct EnumEntry
{
    public string Name { get; init; }
    public long Value { get; init; }
}

/// <summary>The resolved, recursive shape of a script property's type, resolved once per type and shared.</summary>
public sealed class ScriptType
{
    public EScriptKind Kind { get; init; } = EScriptKind.Nil;

    /// <summary>The CLR type this describes.</summary>
    public Type Clr { get; init; } = typeof(object);

    /// <summary>Element shape when <see cref="Kind"/> is <see cref="EScriptKind.Array"/>.</summary>
    public ScriptType? Element { get; init; }

    /// <summary>Members for a NativeStruct or ScriptStruct, round-tripped as a Nested value by member name.</summary>
    public IReadOnlyList<ScriptProperty>? Fields { get; init; }

    /// <summary>Native CStruct name the editor resolves via FindObject.</summary>
    public string? NativeName { get; init; }

    /// <summary>Asset class filter for an AssetRef; empty means any.</summary>
    public string? TargetClass { get; init; }

    public string? EnumName { get; init; }
    public EScriptKind EnumUnderlying { get; init; } = EScriptKind.I32;
    public IReadOnlyList<EnumEntry>? EnumEntries { get; init; }

    /// <summary>The coarse wire kind the value codec uses.</summary>
    public EScriptValueKind ValueKind => Kind switch
    {
        EScriptKind.Bool => EScriptValueKind.Bool,
        EScriptKind.I8 or EScriptKind.I16 or EScriptKind.I32 or EScriptKind.I64 or
        EScriptKind.U8 or EScriptKind.U16 or EScriptKind.U32 or EScriptKind.U64 or
        EScriptKind.Enum or EScriptKind.Entity => EScriptValueKind.Int,
        EScriptKind.F32 or EScriptKind.F64 => EScriptValueKind.Double,
        EScriptKind.String or EScriptKind.AssetRef => EScriptValueKind.String,
        EScriptKind.NativeStruct or EScriptKind.ScriptStruct => EScriptValueKind.Nested,
        EScriptKind.Array => EScriptValueKind.Array,
        _ => EScriptValueKind.Nil,
    };
}

/// <summary>One serializable member of a script type, with name, accessors, type shape, and editor metadata.</summary>
public sealed class ScriptProperty
{
    public string Name { get; init; } = "";
    public ScriptType Type { get; init; } = new();
    public PropertyAttribute? Meta { get; init; }
    /// <summary>Prior member names so a renamed field's saved value still replays.</summary>
    public IReadOnlyList<string>? Aliases { get; init; }
    /// <summary>Reset to default on a hot reload instead of carrying the previous value.</summary>
    public bool SkipHotReload { get; init; }
    public Func<object, object?> Get { get; init; } = Instance => null;
    public Action<object, object?> Set { get; init; } = (Instance, Value) => { };
}

/// <summary>One [Button] method surfaced as an inspector button, invoked by name.</summary>
public sealed class ScriptButton
{
    public string Method { get; init; } = "";
    public string Label { get; init; } = "";
    public string Tooltip { get; init; } = "";
}
