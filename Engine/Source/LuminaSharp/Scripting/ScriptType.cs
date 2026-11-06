using System;
using System.Collections.Generic;

namespace LuminaSharp;

// Mirror of native EPropertyTypeFlags sent as kind bytes, so it only ever appends, checked by LayoutValidator at boot.
public enum EPropertyType : byte
{
    None = 0,
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float, Double,
    Bool, Object, SoftObject, Class, Name, String,
    Enum, Vector, Struct, Optional, SubStruct, Delegate, InstancedStruct, Map,
    Entity, ComponentRef,
}

// The recursive shape of a script property's type, resolved once per type and shared.
public sealed class ScriptType
{
    public EPropertyType Kind { get; init; } = EPropertyType.None;

    public Type Clr { get; init; } = typeof(object);

    // An input binding, stored as its action name and drawn as a picker of the project's actions.
    public bool IsInputAction { get; init; }

    // Set for a Vector or Optional kind.
    public ScriptType? Element { get; init; }

    public ScriptType? KeyType { get; init; }

    public ScriptType? ValueType { get; init; }

    // The native CStruct name the editor resolves through FindObject.
    public string? NativeName { get; init; }

    // The asset class an object reference is filtered to, empty for any.
    public string? TargetClass { get; init; }

    public string? EnumName { get; init; }
    public EPropertyType EnumUnderlying { get; init; } = EPropertyType.Int32;

    // The struct every value of an InstancedStruct kind derives from.
    public string? BaseName { get; init; }
}

// One reflected method, its name and the fields of its call frame.
public sealed class ScriptFunction
{
    public ScriptFunction(string Name, IReadOnlyList<ScriptProperty> Params, int ReturnIndex, uint Flags = 0,
        IReadOnlyList<KeyValuePair<string, string>>? Meta = null)
    {
        this.Name = Name;
        this.Params = Params;
        this.ReturnIndex = ReturnIndex;
        this.Flags = Flags;
        this.Meta = Meta ?? Array.Empty<KeyValuePair<string, string>>();
    }

    // Function metadata native keeps on the FFunction, such as a Button label.
    public IReadOnlyList<KeyValuePair<string, string>> Meta { get; }

    // EFunctionFlags the declaration adds, such as where an [Rpc] method runs.
    public uint Flags { get; }

    public string Name { get; }

    // Arguments in declaration order, the return value last when there is one.
    public IReadOnlyList<ScriptProperty> Params { get; }

    // Index of the return value in Params, or -1 for a function that returns nothing.
    public int ReturnIndex { get; }
}

// One serializable member of a script type, with name, accessors, type shape, and editor metadata.
public sealed class ScriptProperty
{
    public string Name { get; init; } = "";

    // The class that declared the member, so a class minted under a C# parent publishes only its own.
    internal Type? DeclaringType { get; init; }
    public ScriptType Type { get; init; } = new();
    public PropertyAttribute? Meta { get; init; }

    // A [Property(Hidden)], stored and saved but drawn nowhere.
    public bool Hidden { get; init; }

    // Prior member names, so a renamed field's saved value still replays.
    public IReadOnlyList<string>? Aliases { get; init; }
    public bool SkipHotReload { get; init; }

    // The most sends a second a Sync field allows, or zero for every change.
    public float NetRate { get; init; }

    // Metadata keys the C++ spelling of the same declaration would write, such as Sync or AssetType.
    public IReadOnlyList<KeyValuePair<string, string>>? ExtraMeta { get; init; }
    public Func<object, object?> Get { get; init; } = Instance => null;

    // Parameter direction, folded onto the minted property's flags. Always None for a member.
    internal EScriptParamFlags ParamFlags { get; init; }

    // Flags a member earns from its specifiers, such as Replicated from Sync.
    internal EPropertyFlags ExtraFlags { get; init; }
}

