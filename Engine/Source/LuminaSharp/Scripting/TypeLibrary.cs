using System;
using System.Collections.Generic;
using System.Linq.Expressions;
using System.Reflection;
using Lumina;

namespace LuminaSharp;

// The managed reflection registry for one loaded script generation: indexes EntityScript types and resolves
// any type to a cached TypeDescription. Rebuilt wholesale each (re)load, so there is no per-entry invalidation.
internal sealed class TypeLibrary
{
    private readonly Dictionary<string, TypeDescription> EntityScripts = new();
    private readonly Dictionary<string, Type> EntitySystems = new();
    // C# world renderers (RenderScene subclasses); native drives one per Game world when present.
    private readonly Dictionary<string, Type> RenderScenes = new();
    // C# subclasses of REFLECT(Scriptable) native CObjects, keyed by full name; the host mints a CClass per one.
    private readonly Dictionary<string, Type> Scriptables = new();
    // Types carrying a ScriptStructBase marker, keyed by StableId (the simple type name). The host mints a
    // CScriptStruct per one, deriving from the named native struct.
    private readonly Dictionary<string, DataStructEntry> DataStructs = new();
    private readonly Dictionary<Type, TypeDescription> ByType = new();
    // Prior full type names to current full name, so renamed script references resolve.
    private readonly Dictionary<string, string> ScriptAliases = new();
    // Every loaded script type, the universe an instanced field's candidate concrete types are drawn from.
    private readonly List<Type> AllTypes;

    public TypeLibrary(IEnumerable<Type> Types)
    {
        AllTypes = new List<Type>(Types);
        foreach (Type Type in AllTypes)
        {
            if (Type.IsAbstract || Type.FullName is not { } FullName)
            {
                continue;
            }
            if (typeof(EntityScript).IsAssignableFrom(Type))
            {
                EntityScripts[FullName] = Describe(Type);
            }
            else if (typeof(EntitySystem).IsAssignableFrom(Type)
                     && Type.GetCustomAttribute<EntitySystemAttribute>() != null)
            {
                EntitySystems[FullName] = Type;
            }
            else if (typeof(RenderScene).IsAssignableFrom(Type))
            {
                RenderScenes[FullName] = Type;
            }
            else if (IsScriptableSubclass(Type))
            {
                Scriptables[FullName] = Type;
            }
        }

        // A second, unconditional pass rather than another arm of the chain above. A marked data type is
        // not one of those roles and is not required to be reachable from one: it is published because it
        // carries the marker, so discovery cannot depend on the classification or on anything referencing
        // it. Keyed by StableId (simple name) because that is the identity an asset stores, and native
        // struct names share that same flat namespace.
        foreach (Type Type in AllTypes)
        {
            if (Type.IsAbstract)
            {
                continue;
            }

            ScriptStructBaseAttribute? Marker = Type.GetCustomAttribute<ScriptStructBaseAttribute>(inherit: false);
            if (Marker == null)
            {
                continue;
            }

            string StableId = Type.Name;
            if (DataStructs.TryGetValue(StableId, out DataStructEntry Existing))
            {
                Native.Log(ELogLevel.Warn,
                    $"Data type name '{StableId}' is claimed by both '{Existing.Description.Type.FullName}' and "
                    + $"'{Type.FullName}'; ignoring the latter. Rename one, they share one identity namespace.");
                continue;
            }

            DataStructs[StableId] = new DataStructEntry(Describe(Type), Marker.NativeBase);
        }

        // Build the alias map after all current names are known, so an alias never shadows a live type.
        foreach (TypeDescription Description in EntityScripts.Values)
        {
            string Current = Description.Type.FullName!;
            foreach (AliasAttribute Alias in Description.Type.GetCustomAttributes<AliasAttribute>())
            {
                if (string.IsNullOrEmpty(Alias.Name) || EntityScripts.ContainsKey(Alias.Name))
                {
                    continue;
                }
                if (ScriptAliases.TryGetValue(Alias.Name, out string? Existing) && Existing != Current)
                {
                    Native.Log(ELogLevel.Warn, $"[Alias] '{Alias.Name}' is claimed by both '{Existing}' and '{Current}'; ignoring the latter.");
                    continue;
                }
                ScriptAliases[Alias.Name] = Current;
            }
        }
    }

    /// <summary>Full type names of every EntityScript (for the editor's script-picker dropdown).</summary>
    public IReadOnlyCollection<string> EntityScriptTypeNames => EntityScripts.Keys;

    /// <summary>Full type names of every discovered Scriptable C# subclass.</summary>
    public IReadOnlyCollection<string> ScriptableTypeNames => Scriptables.Keys;

    /// <summary>Every discovered Scriptable C# subclass type.</summary>
    public IEnumerable<Type> ScriptableTypes => Scriptables.Values;

    /// <summary>Every marked data type as (StableId, description, native base name).</summary>
    public IEnumerable<KeyValuePair<string, DataStructEntry>> DataStructTypes => DataStructs;

    /// <summary>The description for a marked data type by StableId, or null if unknown.</summary>
    public TypeDescription? GetDataStruct(string StableId)
    {
        return DataStructs.TryGetValue(StableId, out DataStructEntry Entry) ? Entry.Description : null;
    }

    /// <summary>A Scriptable C# subclass by full name, or null if unknown.</summary>
    public Type? GetScriptable(string FullName) => Scriptables.TryGetValue(FullName, out Type? Type) ? Type : null;

    // True if any base type carries [ScriptableType] (Type is a user subclass of a REFLECT(Scriptable) class).
    private static bool IsScriptableSubclass(Type Type)
    {
        for (Type? Base = Type.BaseType; Base != null; Base = Base.BaseType)
        {
            if (Base.GetCustomAttribute<ScriptableTypeAttribute>(false) != null)
            {
                return true;
            }
        }
        return false;
    }

    /// <summary>Every discovered EntitySystem type (carries [EntitySystem]); for the native scheduler.</summary>
    public IReadOnlyCollection<Type> EntitySystemTypes => EntitySystems.Values;

    /// <summary>Every discovered RenderScene subclass; native picks one to render Game worlds with.</summary>
    public IReadOnlyCollection<Type> RenderSceneTypes => RenderScenes.Values;

    /// <summary>A RenderScene type by full name, or null if unknown.</summary>
    public Type? GetRenderScene(string FullName)
    {
        return RenderScenes.TryGetValue(FullName, out Type? Type) ? Type : null;
    }

    /// <summary>An EntitySystem type by full name, or null if unknown.</summary>
    public Type? GetEntitySystem(string FullName)
    {
        return EntitySystems.TryGetValue(FullName, out Type? Type) ? Type : null;
    }

    /// <summary>The description for an EntityScript by full name, falling back through class aliases.</summary>
    public TypeDescription? GetEntityScript(string FullName)
    {
        if (EntityScripts.TryGetValue(FullName, out TypeDescription? Description))
        {
            return Description;
        }
        if (ScriptAliases.TryGetValue(FullName, out string? Current))
        {
            return EntityScripts.TryGetValue(Current, out Description) ? Description : null;
        }
        return null;
    }

    /// <summary>The canonical current full name for a script name, or null if it resolves to no live type.</summary>
    public string? ResolveScriptName(string Name)
    {
        if (EntityScripts.ContainsKey(Name))
        {
            return Name;
        }
        return ScriptAliases.TryGetValue(Name, out string? Current) ? Current : null;
    }

    /// <summary>Get-or-build the description for any type (used recursively for nested struct members).</summary>
    public TypeDescription Describe(Type Type)
    {
        if (ByType.TryGetValue(Type, out TypeDescription? Cached))
        {
            return Cached;
        }

        TypeDescription Description = new(Type);
        ByType[Type] = Description; // insert before building members so self/cyclic references resolve
        Description.Build(this);
        return Description;
    }

    /// <summary>
    /// Resolves a CLR type to its recursive serialized shape, expressed in the shared reflected taxonomy
    /// <see cref="EPropertyType"/>. Scalars/enums map directly; arrays and lists become
    /// <see cref="EPropertyType.Vector"/>; any other struct/class becomes a <see cref="EPropertyType.Struct"/>
    /// whose members recurse. Returns None for shapes we can't serialize. <paramref name="Depth"/> +
    /// <paramref name="Visiting"/> guard against cycles.
    /// </summary>
    public ScriptType ResolveType(Type Type, int Depth, HashSet<Type> Visiting, bool ForceInstanced = false)
    {
        if (Type == typeof(bool))
        {
            return new ScriptType { Kind = EPropertyType.Bool, Clr = Type };
        }
        if (Type == typeof(string))
        {
            return new ScriptType { Kind = EPropertyType.String, Clr = Type };
        }
        // An entity handle is a uint32 tagged so the value codec round-trips it as an Entity and the native
        // editor draws an entity picker; there is no dedicated reflected property type for it.
        if (Type == typeof(Entity))
        {
            return new ScriptType { Kind = EPropertyType.UInt32, Clr = Type, IsEntity = true };
        }
        // An input binding is stored as the name of the action it listens to (the object itself carries the
        // subscriptions, which are code, not data), tagged so the editor draws the action picker. Checked
        // before the generic class branch below, which would otherwise mint its members as a sub-struct.
        if (typeof(InputBinding).IsAssignableFrom(Type) && !Type.IsAbstract)
        {
            return new ScriptType { Kind = EPropertyType.String, Clr = Type, IsInputAction = true };
        }
        if (Type == typeof(float))
        {
            return new ScriptType { Kind = EPropertyType.Float, Clr = Type };
        }
        if (Type == typeof(double))
        {
            return new ScriptType { Kind = EPropertyType.Double, Clr = Type };
        }

        EPropertyType Numeric = MapNumeric(Type);
        if (Numeric != EPropertyType.None)
        {
            return new ScriptType { Kind = Numeric, Clr = Type };
        }

        if (Type.IsEnum)
        {
            EPropertyType Underlying = MapNumeric(Enum.GetUnderlyingType(Type));
            if (Underlying == EPropertyType.None)
            {
                return new ScriptType { Kind = EPropertyType.None, Clr = Type };
            }

            string[] Names = Enum.GetNames(Type);
            Array Values = Enum.GetValues(Type);
            var Entries = new List<EnumEntry>(Names.Length);
            for (int Index = 0; Index < Names.Length; Index++)
            {
                Entries.Add(new EnumEntry { Name = Names[Index], Value = Convert.ToInt64(Values.GetValue(Index)) });
            }

            return new ScriptType
            {
                Kind = EPropertyType.Enum,
                Clr = Type,
                EnumName = Type.FullName ?? Type.Name,
                EnumUnderlying = Underlying,
                EnumEntries = Entries,
            };
        }

        // Asset references round-trip as a path and draw as an asset picker filtered to the target class; they
        // are the script layer's soft-object references (kind SoftObject, distinguished only by TargetClass).
        if (Type == typeof(FSoftObjectPath))
        {
            return new ScriptType { Kind = EPropertyType.SoftObject, Clr = Type, TargetClass = "" };
        }
        if (Type.IsGenericType)
        {
            Type Definition = Type.GetGenericTypeDefinition();
            if (Definition == typeof(TSoftObjectPtr<>) || Definition == typeof(TObjectPtr<>))
            {
                return new ScriptType { Kind = EPropertyType.SoftObject, Clr = Type, TargetClass = Type.GetGenericArguments()[0].Name };
            }
        }

        // A List<T> or T[]. When the field is marked [Instanced], the ELEMENT is the instanced (polymorphic)
        // one, so forward the flag inward: List<ICommand> with [Instanced] picks a concrete type per element.
        if (TryGetElementType(Type, out Type? ElementType))
        {
            ScriptType Element = ResolveType(ElementType!, Depth + 1, Visiting, ForceInstanced);
            if (Element.Kind == EPropertyType.None)
            {
                return new ScriptType { Kind = EPropertyType.None, Clr = Type };
            }
            return new ScriptType { Kind = EPropertyType.Vector, Clr = Type, Element = Element };
        }

        // A Dictionary<K,V> -> a reflected map. Key and value resolve recursively; [Instanced] applies to the
        // value (polymorphic values), never the key. Dropped if either key or value isn't serializable.
        if (TryGetMapTypes(Type, out Type? KeyClr, out Type? ValueClr))
        {
            ScriptType Key = ResolveType(KeyClr!, Depth + 1, Visiting);
            ScriptType Value = ResolveType(ValueClr!, Depth + 1, Visiting, ForceInstanced);
            if (Key.Kind == EPropertyType.None || Value.Kind == EPropertyType.None)
            {
                return new ScriptType { Kind = EPropertyType.None, Clr = Type };
            }
            return new ScriptType { Kind = EPropertyType.Map, Clr = Type, KeyType = Key, ValueType = Value };
        }

        // A C# mirror of a native reflected value struct, drawn by the native CStruct's own customization. Both
        // native and script structs are kind Struct; a non-null NativeName is what marks it as the native one.
        string? NativeName = Type.GetCustomAttribute<NativeTypeAttribute>()?.Name
                           ?? Type.GetCustomAttribute<NativeLayoutAttribute>()?.NativeType;
        if (NativeName != null && Type.IsValueType && Depth < 16 && Visiting.Add(Type))
        {
            try
            {
                List<ScriptProperty> Fields = BuildNativeMembers(Type, Depth, Visiting);
                return new ScriptType { Kind = EPropertyType.Struct, Clr = Type, NativeName = NativeName, Fields = Fields };
            }
            finally
            {
                Visiting.Remove(Type);
            }
        }

        // Instanced (polymorphic) object, opt-in via [Instanced] on the field. Offers a picker of
        // concrete subtypes (and the type itself if concrete). An unmarked field is never instanced.
        if (ForceInstanced && Depth < 16)
        {
            List<ScriptInstanceCandidate> Candidates = DiscoverInstanceCandidates(Type, Depth, Visiting);
            if (Candidates.Count > 0)
            {
                return new ScriptType
                {
                    Kind = EPropertyType.InstancedStruct,
                    Clr = Type,
                    BaseName = Type.Name,
                    Candidates = Candidates,
                };
            }
            return new ScriptType { Kind = EPropertyType.None, Clr = Type };
        }

        // A C#-defined struct or class; its [Property] members are minted into a sub-CScriptStruct. It is a
        // Struct with no NativeName (the marker that separates it from a native-struct mirror above).
        if ((Type.IsClass || (Type.IsValueType && !Type.IsPrimitive)) && Depth < 16 && Visiting.Add(Type))
        {
            try
            {
                List<ScriptProperty> Fields = BuildMembers(Type, Depth, Visiting);
                if (Fields.Count > 0)
                {
                    return new ScriptType { Kind = EPropertyType.Struct, Clr = Type, Fields = Fields };
                }
            }
            finally
            {
                Visiting.Remove(Type);
            }
        }

        return new ScriptType { Kind = EPropertyType.None, Clr = Type };
    }

    /// <summary>The concrete, default-constructible types assignable to Base (and Base itself if concrete),
    /// each resolved to its [Property] members. Drawn from every loaded script type.</summary>
    private List<ScriptInstanceCandidate> DiscoverInstanceCandidates(Type Base, int Depth, HashSet<Type> Visiting)
    {
        var Result = new List<ScriptInstanceCandidate>();
        foreach (Type Candidate in AllTypes)
        {
            if (Candidate.IsAbstract || Candidate.IsInterface || !Base.IsAssignableFrom(Candidate))
            {
                continue;
            }
            // Must be default-constructible: the editor and the deserializer both Activator.CreateInstance it.
            if (Candidate.GetConstructor(System.Type.EmptyTypes) == null)
            {
                continue;
            }
            if (!Visiting.Add(Candidate))
            {
                continue;
            }
            try
            {
                Result.Add(new ScriptInstanceCandidate
                {
                    TypeName = StableTypeName(Candidate),
                    Clr = Candidate,
                    Fields = BuildMembers(Candidate, Depth, Visiting),
                });
            }
            finally
            {
                Visiting.Remove(Candidate);
            }
        }
        Result.Sort((A, B) => string.CompareOrdinal(A.TypeName, B.TypeName));
        return Result;
    }

    // The round-trip key for an instanced candidate; must match on both serialize and deserialize.
    private static string StableTypeName(Type Type) => Type.FullName ?? Type.Name;

    private static EPropertyType MapNumeric(Type Type)
    {
        if (Type == typeof(sbyte))  return EPropertyType.Int8;
        if (Type == typeof(short))  return EPropertyType.Int16;
        if (Type == typeof(int))    return EPropertyType.Int32;
        if (Type == typeof(long))   return EPropertyType.Int64;
        if (Type == typeof(byte))   return EPropertyType.UInt8;
        if (Type == typeof(ushort)) return EPropertyType.UInt16;
        if (Type == typeof(uint))   return EPropertyType.UInt32;
        if (Type == typeof(ulong))  return EPropertyType.UInt64;
        return EPropertyType.None;
    }

    /// <summary>Builds the serializable members of a C#-defined type, every field or property carrying [Property] and not [Hide].</summary>
    internal List<ScriptProperty> BuildMembers(Type Type, int Depth, HashSet<Type> Visiting)
    {
        var Members = new List<ScriptProperty>();
        const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;
        bool bClassSkip = Type.GetCustomAttribute<SkipHotReloadAttribute>() != null;

        foreach (FieldInfo Field in Type.GetFields(Flags))
        {
            PropertyAttribute? Meta = Field.GetCustomAttribute<PropertyAttribute>();
            if (Meta == null || Field.GetCustomAttribute<HideAttribute>() != null)
            {
                continue;
            }

            ScriptType Resolved = ResolveType(Field.FieldType, Depth + 1, Visiting, Field.GetCustomAttribute<InstancedAttribute>() != null);
            if (Resolved.Kind == EPropertyType.None)
            {
                continue;
            }

            Members.Add(new ScriptProperty
            {
                Name = Meta.Name ?? Field.Name,
                Type = Resolved,
                Meta = Meta,
                Aliases = GatherAliases(Field),
                SkipHotReload = bClassSkip || Field.GetCustomAttribute<SkipHotReloadAttribute>() != null,
                Get = Field.GetValue,
                Set = Field.SetValue,
            });
        }

        foreach (PropertyInfo Property in Type.GetProperties(Flags))
        {
            if (!Property.CanRead || !Property.CanWrite || Property.GetIndexParameters().Length > 0)
            {
                continue;
            }

            PropertyAttribute? Meta = Property.GetCustomAttribute<PropertyAttribute>();
            if (Meta == null || Property.GetCustomAttribute<HideAttribute>() != null)
            {
                continue;
            }

            ScriptType Resolved = ResolveType(Property.PropertyType, Depth + 1, Visiting, Property.GetCustomAttribute<InstancedAttribute>() != null);
            if (Resolved.Kind == EPropertyType.None)
            {
                continue;
            }

            Members.Add(new ScriptProperty
            {
                Name = Meta.Name ?? Property.Name,
                Type = Resolved,
                Meta = Meta,
                Aliases = GatherAliases(Property),
                SkipHotReload = bClassSkip || Property.GetCustomAttribute<SkipHotReloadAttribute>() != null,
                Get = Property.GetValue,
                Set = Property.SetValue,
            });
        }

        return Members;
    }

    // Prior member names declared via [Alias], so a renamed field's saved value replays.
    private static IReadOnlyList<string>? GatherAliases(MemberInfo Member)
    {
        List<string>? Result = null;
        foreach (AliasAttribute Alias in Member.GetCustomAttributes<AliasAttribute>())
        {
            if (!string.IsNullOrEmpty(Alias.Name))
            {
                (Result ??= new List<string>()).Add(Alias.Name);
            }
        }
        return Result;
    }

    /// <summary>Members of a C# mirror of a native struct, used only to round-trip its value by member name.</summary>
    private List<ScriptProperty> BuildNativeMembers(Type Type, int Depth, HashSet<Type> Visiting)
    {
        var Members = new List<ScriptProperty>();
        const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.FlattenHierarchy;

        foreach (FieldInfo Field in Type.GetFields(Flags))
        {
            ScriptType Resolved = ResolveType(Field.FieldType, Depth + 1, Visiting);
            if (Resolved.Kind == EPropertyType.None)
            {
                continue;
            }
            Members.Add(new ScriptProperty
            {
                Name = Field.Name,
                Type = Resolved,
                Get = Field.GetValue,
                Set = Field.SetValue,
            });
        }

        foreach (PropertyInfo Property in Type.GetProperties(Flags))
        {
            if (!Property.CanRead || !Property.CanWrite || Property.GetIndexParameters().Length > 0)
            {
                continue;
            }
            ScriptType Resolved = ResolveType(Property.PropertyType, Depth + 1, Visiting);
            if (Resolved.Kind == EPropertyType.None)
            {
                continue;
            }
            Members.Add(new ScriptProperty
            {
                Name = Property.Name,
                Type = Resolved,
                Get = Property.GetValue,
                Set = Property.SetValue,
            });
        }

        return Members;
    }

    private static bool TryGetElementType(Type Type, out Type? ElementType)
    {
        if (Type.IsArray && Type.GetArrayRank() == 1)
        {
            ElementType = Type.GetElementType();
            return ElementType != null;
        }
        if (Type.IsGenericType && Type.GetGenericTypeDefinition() == typeof(List<>))
        {
            ElementType = Type.GetGenericArguments()[0];
            return true;
        }
        ElementType = null;
        return false;
    }

    private static bool TryGetMapTypes(Type Type, out Type? KeyType, out Type? ValueType)
    {
        if (Type.IsGenericType && Type.GetGenericTypeDefinition() == typeof(Dictionary<,>))
        {
            Type[] Args = Type.GetGenericArguments();
            KeyType = Args[0];
            ValueType = Args[1];
            return true;
        }
        KeyType = null;
        ValueType = null;
        return false;
    }
}

// Cached, immutable-after-Build description of one script type: the recursive [Property] member set and the
// precomputed callback bitmask; one description serves every entity carrying the type. Create() uses Activator
// deliberately: a compiled factory would pin this collectible ALC's ctor and block hot-reload unload.
internal sealed class TypeDescription
{
    public Type Type { get; }
    public IReadOnlyList<ScriptProperty> Properties { get; private set; } = Array.Empty<ScriptProperty>();
    public IReadOnlyList<ScriptButton> Buttons { get; private set; } = Array.Empty<ScriptButton>();
    private IReadOnlyList<ScriptProperty> InputBindings = Array.Empty<ScriptProperty>();
    public int CallbackFlags { get; private set; }
    public string ProfileLabel { get; }
    public string FixedProfileLabel { get; }
    private IReadOnlyList<RequiredComponent> RequiredComponents = Array.Empty<RequiredComponent>();

    public TypeDescription(Type Type)
    {
        this.Type = Type;
        ProfileLabel = Type.Name;
        FixedProfileLabel = Type.Name + ".FixedUpdate";
    }

    public void Build(TypeLibrary Library)
    {
        Properties = Library.BuildMembers(Type, 0, new HashSet<Type>());
        Buttons = ComputeButtons(Type);
        InputBindings = ComputeInputBindings(Properties);
        // Bit 5 = "declares input bindings", so native only crosses for scripts that have one. Derived from
        // the members rather than from an override, hence set here and not in ComputeCallbackFlags. The bit
        // index must match the native SCSharpScriptSystem.
        CallbackFlags = ComputeCallbackFlags(Type) | (InputBindings.Count > 0 ? 1 << 5 : 0);
        RequiredComponents = ComputeRequiredComponents(Type);
    }

    // The [Property] members that are input bindings, gathered once per type so the per-frame poll is a
    // list walk with no reflection.
    private static IReadOnlyList<ScriptProperty> ComputeInputBindings(IReadOnlyList<ScriptProperty> Properties)
    {
        List<ScriptProperty>? Result = null;
        foreach (ScriptProperty Property in Properties)
        {
            if (Property.Type.IsInputAction)
            {
                (Result ??= new List<ScriptProperty>()).Add(Property);
            }
        }
        return (IReadOnlyList<ScriptProperty>?)Result ?? Array.Empty<ScriptProperty>();
    }

    /// <summary>Gives a script that declares input bindings the component that feeds them, so declaring an
    /// InputAction field is enough on its own. Same idea as [RequireComponent], and idempotent, so a script
    /// that also calls EnableInput() is unaffected.</summary>
    public void EnsureInputComponent(EntityScript Script)
    {
        if (InputBindings.Count > 0)
        {
            Script.Registry.Emplace<Lumina.SInputComponent>(Script.Entity);
        }
    }

    /// <summary>Feeds this frame's action states to each of the instance's input bindings.</summary>
    public unsafe void PollInputBindings(EntityScript Script, InputActionState* States, int Count, uint Serial, float DeltaTime)
    {
        foreach (ScriptProperty Property in InputBindings)
        {
            // A binding the script nulled out is skipped rather than recreated: the field is the script's
            // to own, and silently handing it a new object would lose whatever it meant by clearing it.
            if (Property.Get(Script) is InputBinding Binding)
            {
                Binding.Poll(States, Count, Serial, DeltaTime);
            }
        }
    }

    // Discovers the [Button] methods: parameterless instance methods surfaced as inspector buttons,
    // invoked by name at click time. Deduped by name (a virtual override would otherwise appear twice).
    private static IReadOnlyList<ScriptButton> ComputeButtons(Type Type)
    {
        const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;
        List<ScriptButton>? Result = null;
        HashSet<string>? Seen = null;

        foreach (MethodInfo Method in Type.GetMethods(Flags))
        {
            ButtonAttribute? Meta = Method.GetCustomAttribute<ButtonAttribute>();
            if (Meta == null)
            {
                continue;
            }
            if (Method.GetParameters().Length != 0 || Method.IsAbstract || Method.IsGenericMethodDefinition)
            {
                Native.Log(ELogLevel.Warn, $"[Button] {Type.Name}.{Method.Name}: only parameterless methods are supported.");
                continue;
            }
            if (!(Seen ??= new HashSet<string>()).Add(Method.Name))
            {
                continue;
            }
            (Result ??= new List<ScriptButton>()).Add(new ScriptButton
            {
                Method = Method.Name,
                Label = string.IsNullOrEmpty(Meta.Label) ? Method.Name : Meta.Label!,
                Tooltip = Meta.Tooltip ?? "",
            });
        }

        return (IReadOnlyList<ScriptButton>?)Result ?? Array.Empty<ScriptButton>();
    }

    /// <summary>Resolves each [RequireComponent] member (adding the component if missing) and assigns the
    /// wrapper before OnReady. Reflection-free per instance, the member set is precomputed.</summary>
    public void InjectRequiredComponents(EntityScript Script)
    {
        if (RequiredComponents.Count == 0)
        {
            return;
        }

        EntityRegistry Registry = Script.Registry;
        foreach (RequiredComponent Required in RequiredComponents)
        {
            IntPtr Pointer = Registry.EmplaceRaw(Script.Entity, Required.Token);
            if (Pointer == IntPtr.Zero)
            {
                Native.Log(ELogLevel.Warn,
                    $"[RequireComponent] {Type.Name}.{Required.MemberName}: '{Required.ComponentName}' is not a registered component.");
                continue;
            }

            // Bind the stored wrapper to the entity so it re-resolves the live component on each access. The
            // field outlives the entt pointer captured here: any later add/remove of this component type can
            // relocate or free the backing slot, so caching the raw pointer would silently dangle (UAF).
            object? View = Required.Factory(Pointer);
            if (View is NativeStruct Component)
            {
                Component.BindToEntity(Registry.WorldHandle, Script.Entity.Id, Required.Token);
            }
            Required.Setter(Script, View);
        }
    }

    // Precompute the [RequireComponent] members: ops token, a compiled (IntPtr)->wrapper factory (over the
    // core-assembly type, so it can't pin the collectible script ALC), and a reflection setter.
    private static IReadOnlyList<RequiredComponent> ComputeRequiredComponents(Type Type)
    {
        const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;
        List<RequiredComponent>? Result = null;

        void Consider(MemberInfo Member, Type MemberType, Action<object, object?> Setter)
        {
            if (Member.GetCustomAttribute<RequireComponentAttribute>() == null)
            {
                return;
            }
            if (!typeof(NativeStruct).IsAssignableFrom(MemberType))
            {
                Native.Log(ELogLevel.Warn,
                    $"[RequireComponent] {Type.Name}.{Member.Name}: type '{MemberType.Name}' is not a component wrapper.");
                return;
            }
            (Result ??= new List<RequiredComponent>()).Add(new RequiredComponent
            {
                MemberName = Member.Name,
                ComponentName = MemberType.Name,
                Token = Native.FindComponentOps(MemberType.Name),
                Factory = BuildWrapperFactory(MemberType),
                Setter = Setter,
            });
        }

        foreach (FieldInfo Field in Type.GetFields(Flags))
        {
            Consider(Field, Field.FieldType, Field.SetValue);
        }
        foreach (PropertyInfo Property in Type.GetProperties(Flags))
        {
            if (Property.CanWrite && Property.GetIndexParameters().Length == 0)
            {
                Consider(Property, Property.PropertyType, Property.SetValue);
            }
        }

        return (IReadOnlyList<RequiredComponent>?)Result ?? Array.Empty<RequiredComponent>();
    }

    private static Func<IntPtr, object?> BuildWrapperFactory(Type WrapperType)
    {
        ConstructorInfo? Constructor = WrapperType.GetConstructor(
            BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public,
            null, new[] { typeof(IntPtr) }, null);
        if (Constructor == null)
        {
            return static _ => null;
        }
        ParameterExpression Parameter = Expression.Parameter(typeof(IntPtr), "handle");
        return Expression.Lambda<Func<IntPtr, object?>>(
            Expression.Convert(Expression.New(Constructor, Parameter), typeof(object)), Parameter).Compile();
    }

    public object? Create()
    {
        if (Type.IsAbstract || Type.IsInterface)
        {
            return null;
        }

        try
        {
            return Activator.CreateInstance(Type);
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"Failed to instantiate script '{Type.FullName}': {Exception}");
            return null;
        }
    }

    private static int ComputeCallbackFlags(Type Type)
    {
        // Bit indices must match the native CSharpScriptSystem.
        int Flags = 0;

        // OnInput (bit 4).
        MethodInfo? Input = Type.GetMethod("OnInput",
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
            null, new[] { typeof(Lumina.InputEvent) }, null);
        if (Input != null && Input.DeclaringType != typeof(EntityScript))
        {
            Flags |= 1 << 4;
        }

        // OnFixedUpdate (bit 9): runs at the fixed physics timestep. Only overriding scripts are dispatched.
        MethodInfo? Fixed = Type.GetMethod("OnFixedUpdate",
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
            null, new[] { typeof(float) }, null);
        if (Fixed != null && Fixed.DeclaringType != typeof(EntityScript))
        {
            Flags |= 1 << 9;
        }

        // OnUpdate (bit 10).
        MethodInfo? Update = Type.GetMethod("OnUpdate",
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
            null, new[] { typeof(float) }, null);
        if (Update != null && Update.DeclaringType != typeof(EntityScript))
        {
            Flags |= 1 << 10;
        }

        // Update phase (bit 16): a [UpdatePhase(EScriptPhase.PostPhysics)] script runs its OnUpdate in the
        // PostPhysics group instead of the default PrePhysics group. This bit index must match the native
        // SCSharpScriptSystem (PostPhysicsPhaseBit). Folded into the callback flags so it rides the existing
        // GetScriptCallbackFlags path onto the native component with no extra crossing or ABI change.
        if (Type.GetCustomAttribute<UpdatePhaseAttribute>() is { Phase: EScriptPhase.PostPhysics })
        {
            Flags |= 1 << 16;
        }

        return Flags;
    }
}

/// <summary>One precomputed [RequireComponent] member: ops token, wrapper factory, and the setter.</summary>
internal sealed class RequiredComponent
{
    public required string MemberName;
    public required string ComponentName;
    public required IntPtr Token;
    public required Func<IntPtr, object?> Factory;
    public required Action<object, object?> Setter;
}

/// <summary>A discovered data type: its member description plus the native struct it derives from.</summary>
internal readonly struct DataStructEntry
{
    public DataStructEntry(TypeDescription Description, string NativeBase)
    {
        this.Description = Description;
        this.NativeBase = NativeBase;
    }

    public TypeDescription Description { get; }

    /// <summary>Registered name of the native CStruct the minted type is given as its super.</summary>
    public string NativeBase { get; }
}
