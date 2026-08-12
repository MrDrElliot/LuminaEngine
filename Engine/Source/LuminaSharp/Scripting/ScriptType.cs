using System;
using System.Collections.Generic;

namespace LuminaSharp;

/// <summary>
/// C# mirror of the engine's reflected type taxonomy <c>Lumina::EPropertyTypeFlags</c> (ObjectCore.h). The
/// script schema keys on this single shared enum instead of a parallel one, so there is nothing to keep in
/// sync by hand; <see cref="LayoutValidator"/> asserts the values match native at bootstrap. Order IS the wire
/// ABI (the schema sends these as kind bytes), so only ever append. Script-specific shapes are carried as data
/// rather than distinct kinds: an entity is <c>UInt32</c> + <see cref="ScriptType.IsEntity"/>; an asset ref is
/// <c>SoftObject</c> + TargetClass; a native vs. script struct is <c>Struct</c> distinguished by NativeName.
/// </summary>
public enum EPropertyType : byte
{
    None = 0,
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float, Double,
    Bool, Object, SoftObject, Class, Name, String,
    Enum, Vector, Struct, Optional, SubStruct, Delegate, InstancedStruct, Map,
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
    Instance,
    Map,   // count (i32) followed by that many [key value, value value] pairs. Append-only (persisted wire).
}

/// <summary>One selectable concrete type for an instanced field, with its stable name, CLR type, and members.</summary>
public sealed class ScriptInstanceCandidate
{
    public string TypeName { get; init; } = "";
    public Type Clr { get; init; } = typeof(object);
    public IReadOnlyList<ScriptProperty> Fields { get; init; } = Array.Empty<ScriptProperty>();
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
    public EPropertyType Kind { get; init; } = EPropertyType.None;

    /// <summary>The CLR type this describes.</summary>
    public Type Clr { get; init; } = typeof(object);

    /// <summary>True when this is a <see cref="Entity"/> handle (kind <see cref="EPropertyType.UInt32"/> carries
    /// no entity marker of its own, so this rides alongside so the value codec and the native entity picker both
    /// see it).</summary>
    public bool IsEntity { get; init; }

    /// <summary>True when this is an <see cref="InputBinding"/> (kind <see cref="EPropertyType.String"/>): the
    /// value round-trips as the action name, and the native editor draws a picker of the project's actions.</summary>
    public bool IsInputAction { get; init; }

    /// <summary>Element shape when <see cref="Kind"/> is <see cref="EPropertyType.Vector"/>.</summary>
    public ScriptType? Element { get; init; }

    /// <summary>Key shape when <see cref="Kind"/> is <see cref="EPropertyType.Map"/>.</summary>
    public ScriptType? KeyType { get; init; }

    /// <summary>Value shape when <see cref="Kind"/> is <see cref="EPropertyType.Map"/>.</summary>
    public ScriptType? ValueType { get; init; }

    /// <summary>Members for a NativeStruct or ScriptStruct, round-tripped as a Nested value by member name.</summary>
    public IReadOnlyList<ScriptProperty>? Fields { get; init; }

    /// <summary>Native CStruct name the editor resolves via FindObject.</summary>
    public string? NativeName { get; init; }

    /// <summary>Asset class filter for an AssetRef; empty means any.</summary>
    public string? TargetClass { get; init; }

    public string? EnumName { get; init; }
    public EPropertyType EnumUnderlying { get; init; } = EPropertyType.Int32;
    public IReadOnlyList<EnumEntry>? EnumEntries { get; init; }

    /// <summary>Display name of the base type when <see cref="Kind"/> is <see cref="EPropertyType.InstancedStruct"/>.</summary>
    public string? BaseName { get; init; }

    /// <summary>Selectable concrete types when <see cref="Kind"/> is <see cref="EPropertyType.InstancedStruct"/>.</summary>
    public IReadOnlyList<ScriptInstanceCandidate>? Candidates { get; init; }

    /// <summary>The coarse wire kind the value codec uses, projected from the reflected property type.</summary>
    public EScriptValueKind ValueKind => Kind switch
    {
        EPropertyType.Bool => EScriptValueKind.Bool,
        EPropertyType.Int8 or EPropertyType.Int16 or EPropertyType.Int32 or EPropertyType.Int64 or
        EPropertyType.UInt8 or EPropertyType.UInt16 or EPropertyType.UInt32 or EPropertyType.UInt64 or
        EPropertyType.Enum => EScriptValueKind.Int,
        EPropertyType.Float or EPropertyType.Double => EScriptValueKind.Double,
        EPropertyType.String or EPropertyType.SoftObject => EScriptValueKind.String,
        EPropertyType.Struct => EScriptValueKind.Nested,
        EPropertyType.Vector => EScriptValueKind.Array,
        EPropertyType.Map => EScriptValueKind.Map,
        EPropertyType.InstancedStruct => EScriptValueKind.Instance,
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

    /// <summary>
    /// True when the member is a get-only VIEW over storage native already owns -- a container property on a
    /// script type, which hands out a <see cref="NativeList{T}"/> rather than a managed copy.
    ///
    /// Such a member is still a real reflected property (native holds the container and the inspector edits
    /// it); what it has no meaning for is the two things that assume a managed-side value: capturing a
    /// declared default off an unbound instance, and assigning a decoded value back through a setter. Both
    /// are skipped for it, which is why "no setter" cannot simply mean "not a property".
    /// </summary>
    public bool IsNativeOwnedView { get; init; }
}

/// <summary>One [Button] method surfaced as an inspector button, invoked by name.</summary>
public sealed class ScriptButton
{
    public string Method { get; init; } = "";
    public string Label { get; init; } = "";
    public string Tooltip { get; init; } = "";
}
