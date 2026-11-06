using System;
using JetBrains.Annotations;
using Lumina;
using System.Collections.Generic;
using System.Reflection;

namespace LuminaSharp;

// The C# spelling of a C++ REFLECT(...), and what registers a class extending a native struct, as REFLECT does for a C++ struct.
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct | AttributeTargets.Enum, AllowMultiple = false, Inherited = false)]
public sealed class ReflectAttribute : Attribute
{
    private EScriptUpdatePhase UpdatePhaseValue;
    private bool InteractiveValue;
    private bool ShowOnStartValue = true;

    public string? DisplayName { get; set; }

    public string? Category { get; set; }

    public string? ToolTip { get; set; }

    // The name an .rml's data-model attribute uses for this class. The class name when no class in the chain sets one.
    public string? DataModel { get; set; }

    // The .rml a UI script shows when the entity leaves its Document property empty.
    public string? UIDocument { get; set; }

    // Backs a settings class with this config file, which the config system loads and saves its properties to.
    public string? ConfigFile { get; set; }

    // Puts every asset of this class and its subclasses in the named asset group, the same as a C++ REFLECT(Group = ...).
    public string? Group { get; set; }

    public bool HideInComponentList { get; set; }

    public bool HideInDetails { get; set; }

    // Keeps the class out of the node graph's placeable node registry.
    public bool NotPlaceable { get; set; }

    // OnUpdate and OnFixedUpdate touch only this entity and change no structure, so the driver runs it in parallel.
    public bool ParallelUpdate { get; set; }

    // Runs only on the host, a dedicated server or a standalone game. Every realm set must hold.
    public bool HostOnly { get; set; }

    // Runs only on a client joined to a host, never on the host or in a standalone game.
    public bool ClientOnly { get; set; }

    // Runs only where something is drawn or played, never on a dedicated server, a bot or a headless process.
    public bool Cosmetic { get; set; }

    public EScriptUpdatePhase UpdatePhase
    {
        get => UpdatePhaseValue;
        set
        {
            UpdatePhaseValue = value;
            HasUpdatePhase = true;
        }
    }

    // A UI script's starting Interactive value, which a C++ subclass would set in its constructor.
    public bool Interactive
    {
        get => InteractiveValue;
        set
        {
            InteractiveValue = value;
            HasInteractive = true;
        }
    }

    // A UI script's starting ShowOnStart value, which a C++ subclass would set in its constructor.
    public bool ShowOnStart
    {
        get => ShowOnStartValue;
        set
        {
            ShowOnStartValue = value;
            HasShowOnStart = true;
        }
    }

    internal bool HasUpdatePhase { get; private set; }

    internal bool HasInteractive { get; private set; }

    internal bool HasShowOnStart { get; private set; }

    // The nearest [Reflect] up Type's chain that sets what Picks reads, mirroring how native reads class metadata.
    internal static ReflectAttribute? Nearest(Type? Type, Func<ReflectAttribute, bool> Picks)
    {
        for (Type? Current = Type; Current != null; Current = Current.BaseType)
        {
            if (Current.GetCustomAttribute<ReflectAttribute>(inherit: false) is { } Reflect && Picks(Reflect))
            {
                return Reflect;
            }
        }
        return null;
    }

    internal static string DataModelOf(Type Type) => Nearest(Type, Reflect => Reflect.DataModel != null)?.DataModel ?? Type.Name;

    // Specifier name and value as C++ REFLECT spells them, with an empty value for a flag.
    internal IEnumerable<KeyValuePair<string, string>> Specifiers()
    {
        if (DisplayName != null)
        {
            yield return new(nameof(DisplayName), DisplayName);
        }
        if (Category != null)
        {
            yield return new(nameof(Category), Category);
        }
        if (ToolTip != null)
        {
            yield return new(nameof(ToolTip), ToolTip);
        }
        if (DataModel != null)
        {
            yield return new(nameof(DataModel), DataModel);
        }
        if (UIDocument != null)
        {
            yield return new(nameof(UIDocument), UIDocument);
        }
        if (ConfigFile != null)
        {
            yield return new(nameof(ConfigFile), ConfigFile);
        }
        if (Group != null)
        {
            yield return new(nameof(Group), Group);
        }
        if (HideInComponentList)
        {
            yield return new(nameof(HideInComponentList), "");
        }
        if (HideInDetails)
        {
            yield return new(nameof(HideInDetails), "");
        }
        if (NotPlaceable)
        {
            yield return new(nameof(NotPlaceable), "");
        }
        if (ParallelUpdate)
        {
            yield return new(nameof(ParallelUpdate), "");
        }
        if (HostOnly)
        {
            yield return new(nameof(HostOnly), "");
        }
        if (ClientOnly)
        {
            yield return new(nameof(ClientOnly), "");
        }
        if (Cosmetic)
        {
            yield return new(nameof(Cosmetic), "");
        }
        if (HasUpdatePhase)
        {
            yield return new(nameof(UpdatePhase), UpdatePhase.ToString());
        }
    }
}

// Class metadata an engine C# base hands to every script class minted beneath it, for a key no REFLECT specifier spells.
[AttributeUsage(AttributeTargets.Class, AllowMultiple = true, Inherited = false)]
internal sealed class ClassMetaAttribute : Attribute
{
    public ClassMetaAttribute(string Key, string Value = "")
    {
        this.Key = Key;
        this.Value = Value;
    }

    public string Key { get; }

    public string Value { get; }
}

// Dispatches Event to a script type that does not override it when the static bool(TypeLibrary, Type) Predicate on this class accepts the type, for engine work in a pre-event hook.
[AttributeUsage(AttributeTargets.Class, AllowMultiple = true, Inherited = false)]
internal sealed class DispatchWhenAttribute : Attribute
{
    public DispatchWhenAttribute(string Event, string Predicate)
    {
        this.Event = Event;
        this.Predicate = Predicate;
    }

    public string Event { get; }

    public string Predicate { get; }
}

// A prior name this element was saved under, so saved data survives a rename.
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct | AttributeTargets.Enum | AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = true, Inherited = false)]
public sealed class AliasAttribute : Attribute
{
    public AliasAttribute(string Name)
    {
        this.Name = Name;
    }

    // The old type name on a class, struct or enum, or the old member name on a property.
    public string Name { get; }
}

// Resets this [Property], or every one on the class, to its default on a hot reload instead of carrying the old value over.
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = false)]
public sealed class SkipHotReloadAttribute : Attribute
{
}

// Mirror of native EPropertyFlags, checked per member at boot since a wrong bit silently stops a field replicating.
[Flags]
public enum EPropertyFlags : uint
{
    None               = 0,

    Editable           = 1u << 0,
    ReadOnly           = 1u << 1,
    NoSerialize        = 1u << 2,
    // Not writable through reflection at all.
    Const              = 1u << 3,
    // Stripped from cooked packages.
    EditorOnly         = 1u << 11,
    Replicated         = 1u << 12,
    // Duplication resets this to its default rather than copying it.
    DuplicateTransient = 1u << 16,
    SaveGame           = 1u << 19,
}

// The parameter half of native EPropertyFlags, kept apart because neither is anything a [Property] declares.
[Flags]
internal enum EScriptParamFlags : uint
{
    None     = 0,

    // The caller reads this frame slot back after the call, which is what the dispatcher writes back into.
    OutParam = 1u << 17,

    // An out parameter the callee also sees the incoming value of, so C# ref rather than C# out.
    RefParam = 1u << 18,
}

// How a [Property(Component = ...)] reference is filled before OnAttach.
public enum EComponentBind
{
    None,

    // Finds the entity's component, including one added after the script attaches.
    Get,

    // Adds the component first when the entity has none.
    GetOrAdd,
}

// The bare words a [Property] or [Function] takes, as C++ PROPERTY(...) and FUNCTION(...) do, imported into every script.
public enum ESpecifier
{
    Editable,
    ReadOnly,
    Hidden,
    NoSerialize,
    SaveGame,
    EditorOnly,
    DuplicateTransient,
    Sync,
    Bind,

    // C++ spells this Color, which a bare name in C# cannot, since it would collide with LuminaSharp.Color.
    ColorPicker,

    Button,
}

// The C# spelling of a C++ PROPERTY(...), which stores the member natively, saves it and shows it as Editable unless it says ReadOnly or Hidden.
[MeansImplicitUse(ImplicitUseKindFlags.Access | ImplicitUseKindFlags.Assign)]
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = false)]
public sealed class PropertyAttribute : Attribute
{
    private readonly ESpecifier[] Specifiers;
    private Lumina.ESyncFlags SyncValue;
    private bool bSyncNamed;

    public PropertyAttribute(params ESpecifier[] Specifiers)
    {
        this.Specifiers = Specifiers;
    }

    public string? Category { get; set; }

    public string? ToolTip { get; set; }

    public string? DisplayName { get; set; }

    public float ClampMin { get; set; } = float.NaN;

    public float ClampMax { get; set; } = float.NaN;

    public string? Units { get; set; }

    public string? AssetType { get; set; }

    public string? Bundle { get; set; }

    public string? Group { get; set; }

    // On an instanced struct, limits the type picker to script structs that implement this interface.
    public Type? Implements { get; set; }

    public EComponentBind Component { get; set; }

    // The name a UI document binds this under, which implies Bind.
    public string? Bind { get; set; }

    // Implies Sync, and FromOwner lets the owning client write the field too.
    public Lumina.ESyncFlags Sync
    {
        get => SyncValue;
        set
        {
            SyncValue = value;
            bSyncNamed = true;
        }
    }

    public float Rate { get; set; }

    public double Quantize { get; set; }

    public string? Validate { get; set; }

    public string? Change { get; set; }

    public bool HasClampMin => !float.IsNaN(ClampMin);
    public bool HasClampMax => !float.IsNaN(ClampMax);

    public bool Has(ESpecifier Specifier)
    {
        return Array.IndexOf(Specifiers, Specifier) >= 0
            || (Specifier == ESpecifier.Sync && bSyncNamed)
            || (Specifier == ESpecifier.Bind && Bind != null);
    }

    public bool Has(string Specifier) => Enum.TryParse(Specifier, out ESpecifier Parsed) && Has(Parsed);
}

// A native reflected virtual a C# subclass of a Scriptable class may override, emitted by the Reflector.
[AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
public sealed class ScriptEventAttribute : Attribute
{
    public ScriptEventAttribute(int Index)
    {
        this.Index = Index;
    }

    // Matches the native shim's per-method bit, so native learns which events the subclass overrides.
    public int Index { get; }
}

// The C# spelling of a C++ FUNCTION(...), which publishes the method as a reflected function native and other scripts call by name.
[MeansImplicitUse]
[AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
public sealed class FunctionAttribute : Attribute
{
    private readonly ESpecifier[] Specifiers;

    public FunctionAttribute(params ESpecifier[] Specifiers)
    {
        this.Specifiers = Specifiers;
    }

    public string? ToolTip { get; set; }

    // The label of the inspector button that calls this parameterless method, which implies Button.
    public string? Button { get; set; }

    // The name a UI document runs or reads this under, which implies Bind.
    public string? Bind { get; set; }

    public bool Has(ESpecifier Specifier)
    {
        return Array.IndexOf(Specifiers, Specifier) >= 0
            || (Specifier == ESpecifier.Button && Button != null)
            || (Specifier == ESpecifier.Bind && Bind != null);
    }
}
