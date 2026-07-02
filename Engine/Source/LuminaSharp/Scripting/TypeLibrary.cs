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
    // C# subclasses of REFLECT(Scriptable) native CObjects, keyed by full name; the host mints a CClass per one.
    private readonly Dictionary<string, Type> Scriptables = new();
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
            else if (IsScriptableSubclass(Type))
            {
                Scriptables[FullName] = Type;
            }
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
    /// Resolves a CLR type to its recursive serialized shape. Scalars/vectors/enums map directly;
    /// arrays and lists become <see cref="EScriptKind.Array"/>; any other struct/class becomes a
    /// <see cref="EScriptKind.NestedStruct"/> whose members recurse. Returns Nil for shapes we can't
    /// serialize. <paramref name="Depth"/> + <paramref name="Visiting"/> guard against cycles.
    /// </summary>
    public ScriptType ResolveType(Type Type, int Depth, HashSet<Type> Visiting, bool ForceInstanced = false)
    {
        if (Type == typeof(bool))
        {
            return new ScriptType { Kind = EScriptKind.Bool, Clr = Type };
        }
        if (Type == typeof(string))
        {
            return new ScriptType { Kind = EScriptKind.String, Clr = Type };
        }
        if (Type == typeof(Entity))
        {
            return new ScriptType { Kind = EScriptKind.Entity, Clr = Type };
        }
        if (Type == typeof(float))
        {
            return new ScriptType { Kind = EScriptKind.F32, Clr = Type };
        }
        if (Type == typeof(double))
        {
            return new ScriptType { Kind = EScriptKind.F64, Clr = Type };
        }

        EScriptKind Numeric = MapNumeric(Type);
        if (Numeric != EScriptKind.Nil)
        {
            return new ScriptType { Kind = Numeric, Clr = Type };
        }

        if (Type.IsEnum)
        {
            EScriptKind Underlying = MapNumeric(Enum.GetUnderlyingType(Type));
            if (Underlying == EScriptKind.Nil)
            {
                return new ScriptType { Kind = EScriptKind.Nil, Clr = Type };
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
                Kind = EScriptKind.Enum,
                Clr = Type,
                EnumName = Type.FullName ?? Type.Name,
                EnumUnderlying = Underlying,
                EnumEntries = Entries,
            };
        }

        // Asset references round-trip as a path and draw as an asset picker filtered to the target class.
        if (Type == typeof(FSoftObjectPath))
        {
            return new ScriptType { Kind = EScriptKind.AssetRef, Clr = Type, TargetClass = "" };
        }
        if (Type.IsGenericType)
        {
            Type Definition = Type.GetGenericTypeDefinition();
            if (Definition == typeof(TSoftObjectPtr<>) || Definition == typeof(TObjectPtr<>))
            {
                return new ScriptType { Kind = EScriptKind.AssetRef, Clr = Type, TargetClass = Type.GetGenericArguments()[0].Name };
            }
        }

        // A List<T> or T[]. When the field is marked [Instanced], the ELEMENT is the instanced (polymorphic)
        // one, so forward the flag inward: List<ICommand> with [Instanced] picks a concrete type per element.
        if (TryGetElementType(Type, out Type? ElementType))
        {
            ScriptType Element = ResolveType(ElementType!, Depth + 1, Visiting, ForceInstanced);
            if (Element.Kind == EScriptKind.Nil)
            {
                return new ScriptType { Kind = EScriptKind.Nil, Clr = Type };
            }
            return new ScriptType { Kind = EScriptKind.Array, Clr = Type, Element = Element };
        }

        // A C# mirror of a native reflected value struct, drawn by the native CStruct's own customization.
        string? NativeName = Type.GetCustomAttribute<NativeTypeAttribute>()?.Name
                           ?? Type.GetCustomAttribute<NativeLayoutAttribute>()?.NativeType;
        if (NativeName != null && Type.IsValueType && Depth < 16 && Visiting.Add(Type))
        {
            try
            {
                List<ScriptProperty> Fields = BuildNativeMembers(Type, Depth, Visiting);
                return new ScriptType { Kind = EScriptKind.NativeStruct, Clr = Type, NativeName = NativeName, Fields = Fields };
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
                    Kind = EScriptKind.Instance,
                    Clr = Type,
                    BaseName = Type.Name,
                    Candidates = Candidates,
                };
            }
            return new ScriptType { Kind = EScriptKind.Nil, Clr = Type };
        }

        // A C#-defined struct or class; its [Property] members are minted into a sub-CScriptStruct.
        if ((Type.IsClass || (Type.IsValueType && !Type.IsPrimitive)) && Depth < 16 && Visiting.Add(Type))
        {
            try
            {
                List<ScriptProperty> Fields = BuildMembers(Type, Depth, Visiting);
                if (Fields.Count > 0)
                {
                    return new ScriptType { Kind = EScriptKind.ScriptStruct, Clr = Type, Fields = Fields };
                }
            }
            finally
            {
                Visiting.Remove(Type);
            }
        }

        return new ScriptType { Kind = EScriptKind.Nil, Clr = Type };
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

    private static EScriptKind MapNumeric(Type Type)
    {
        if (Type == typeof(sbyte))  return EScriptKind.I8;
        if (Type == typeof(short))  return EScriptKind.I16;
        if (Type == typeof(int))    return EScriptKind.I32;
        if (Type == typeof(long))   return EScriptKind.I64;
        if (Type == typeof(byte))   return EScriptKind.U8;
        if (Type == typeof(ushort)) return EScriptKind.U16;
        if (Type == typeof(uint))   return EScriptKind.U32;
        if (Type == typeof(ulong))  return EScriptKind.U64;
        return EScriptKind.Nil;
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
            if (Resolved.Kind == EScriptKind.Nil)
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
            if (Resolved.Kind == EScriptKind.Nil)
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
            if (Resolved.Kind == EScriptKind.Nil)
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
            if (Resolved.Kind == EScriptKind.Nil)
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
}

// Cached, immutable-after-Build description of one script type: the recursive [Property] member set and the
// precomputed callback bitmask; one description serves every entity carrying the type. Create() uses Activator
// deliberately: a compiled factory would pin this collectible ALC's ctor and block hot-reload unload.
internal sealed class TypeDescription
{
    public Type Type { get; }
    public IReadOnlyList<ScriptProperty> Properties { get; private set; } = Array.Empty<ScriptProperty>();
    public IReadOnlyList<ScriptButton> Buttons { get; private set; } = Array.Empty<ScriptButton>();
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
        CallbackFlags = ComputeCallbackFlags(Type);
        RequiredComponents = ComputeRequiredComponents(Type);
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
