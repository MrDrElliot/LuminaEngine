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
        // Before anything is described: proves the view table below still lines up with the shared classifier
        // the rewriter emitted accessors from. Cheap, once, and the failures it catches are otherwise silent.
        ScriptPropertyViews.Validate();

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
            else if (typeof(EntitySystem).IsAssignableFrom(Type))
            {
                ++EntitySystemCount;
            }

            // NOT an "else": the roles above describe what a type is FOR, and being a Scriptable is a
            // separate fact about how it EXISTS -- as a CObject of a minted CClass deriving the native base.
            // An EntityScript is both, and has been since it became a Lumina.CEntityScript: it is the mint
            // that gives it a class to instantiate and the trailing block its [Property] members live in. As
            // an exclusive chain this silently published no EntityScript to the minter, so no script class
            // existed and no script property was ever appended.
            if (IsScriptableSubclass(Type))
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
        //
        // Over every Scriptable, not just EntityScripts: the map is what tells native where a renamed script
        // CLASS went, and any Scriptable can be minted and attached. An EntityScript is a Scriptable too, so
        // this is a superset of what it used to walk.
        foreach (Type Scriptable in Scriptables.Values)
        {
            string Current = Scriptable.FullName!;
            foreach (AliasAttribute Alias in Scriptable.GetCustomAttributes<AliasAttribute>())
            {
                if (string.IsNullOrEmpty(Alias.Name)
                    || EntityScripts.ContainsKey(Alias.Name)
                    || Scriptables.ContainsKey(Alias.Name))
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

    /// <summary>Prior script class name to its current full name, from the <c>[Alias]</c> attributes on
    /// script classes. Native mirrors this into its class-redirect registry so a renamed class still resolves
    /// from a saved scene and so a hot reload can move live instances onto the new class.</summary>
    public IEnumerable<KeyValuePair<string, string>> ClassAliases => ScriptAliases;

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
    /// <summary>A type that IS a view over native storage rather than a managed value, so a get-only member
    /// of it is still a real property. One of three questions asked of ScriptPropertyViews' single table --
    /// the others being element and key/value -- so a view cannot be known to one and not the others.</summary>
    private static bool IsNativeOwnedViewType(Type Type) => ScriptPropertyViews.IsView(Type);

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

    // Discovery is the native class walk, so this only feeds the scripting diagnostics report.
    public int EntitySystemCount { get; private set; }

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
        // An interned name. POD, so it round-trips as its own reflected kind rather than as text: native
        // mints an FNameProperty and C# reads the id in place.
        if (Type == typeof(FName))
        {
            return new ScriptType { Kind = EPropertyType.Name, Clr = Type };
        }
        // Both string spellings are one native FString. FString is the explicit mirror -- the only one that
        // can be a container ELEMENT, since a managed reference cannot live in native memory.
        if (Type == typeof(string) || Type == typeof(FString))
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
        if (typeof(SInputBinding).IsAssignableFrom(Type) && !Type.IsAbstract)
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
        // A nullable is an optional over its payload, matching the native TOptional a C++ one reflects as.
        if (Nullable.GetUnderlyingType(Type) is Type Payload)
        {
            ScriptType Inner = ResolveType(Payload, Depth + 1, Visiting);
            if (Inner.Kind == EPropertyType.None)
            {
                return new ScriptType { Kind = EPropertyType.None, Clr = Type };
            }
            return new ScriptType { Kind = EPropertyType.Optional, Clr = Type, Element = Inner };
        }

        if (Type.IsGenericType)
        {
            Type Definition = Type.GetGenericTypeDefinition();
            // A SOFT reference is a path resolved on demand; a HARD one is a live object pointer that keeps
            // its target alive. They were both reported as SoftObject, which gave TObjectPtr<T> an asset
            // picker, no strong reference, and no way to point at an object that has no asset path.
            if (Definition == typeof(TSoftObjectPtr<>))
            {
                return new ScriptType { Kind = EPropertyType.SoftObject, Clr = Type, TargetClass = Type.GetGenericArguments()[0].Name };
            }
            if (Definition == typeof(TObjectPtr<>))
            {
                return new ScriptType { Kind = EPropertyType.Object, Clr = Type, TargetClass = Type.GetGenericArguments()[0].Name };
            }
        }

        // A wrapper around a native CObject (CTexture, CAudioStream, ...): a HARD reference, stored as an
        // object property. Checked before the generic class branch far below, which would otherwise mint the
        // wrapper's own members as a sub-struct -- native would hold a struct while C# read an object pointer.
        if (typeof(NativeObject).IsAssignableFrom(Type))
        {
            return new ScriptType { Kind = EPropertyType.Object, Clr = Type, TargetClass = Type.Name };
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
                    return new ScriptType
                    {
                        Kind = EPropertyType.Struct,
                        Clr = Type,
                        Fields = Fields,
                        ManagedSize = ManagedSizeOf(Type),
                    };
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

    private static readonly MethodInfo UnsafeSizeOf =
        typeof(System.Runtime.CompilerServices.Unsafe).GetMethod(
            nameof(System.Runtime.CompilerServices.Unsafe.SizeOf),
            BindingFlags.Public | BindingFlags.Static)!;

    // The managed size, which is what the generated accessor reads, not the marshalled one Marshal.SizeOf gives.
    private static int ManagedSizeOf(Type Type)
    {
        if (!Type.IsValueType)
        {
            return 0;
        }
        try
        {
            return (int)UnsafeSizeOf.MakeGenericMethod(Type).Invoke(null, null)!;
        }
        catch (Exception Ex)
        {
            Native.Log(ELogLevel.Warn, $"Could not size script struct '{Type.FullName}': {Ex.Message}");
            return 0;
        }
    }

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

    // Every field or property carrying [Property] or [Serialize] and not [Hide]. [Serialize] is stored but not drawn.
    internal List<ScriptProperty> BuildMembers(Type Type, int Depth, HashSet<Type> Visiting)
    {
        var Members = new List<ScriptProperty>();
        const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;
        bool bClassSkip = Type.GetCustomAttribute<SkipHotReloadAttribute>() != null;

        foreach (FieldInfo Field in Type.GetFields(Flags))
        {
            PropertyAttribute? Meta = Field.GetCustomAttribute<PropertyAttribute>();
            bool bSerializeOnly = Meta == null && Field.GetCustomAttribute<SerializeAttribute>() != null;
            if ((Meta == null && !bSerializeOnly) || Field.GetCustomAttribute<HideAttribute>() != null)
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
                Name = Meta?.Name ?? Field.Name,
                Type = Resolved,
                Meta = Meta,
                Hidden = bSerializeOnly,
                Aliases = GatherAliases(Field),
                SkipHotReload = bClassSkip || Field.GetCustomAttribute<SkipHotReloadAttribute>() != null,
                Get = Field.GetValue,
                Set = Field.SetValue,
            });
        }

        foreach (PropertyInfo Property in Type.GetProperties(Flags))
        {
            if (!Property.CanRead || Property.GetIndexParameters().Length > 0)
            {
                continue;
            }

            PropertyAttribute? Meta = Property.GetCustomAttribute<PropertyAttribute>();
            bool bSerializeOnly = Meta == null && Property.GetCustomAttribute<SerializeAttribute>() != null;
            if ((Meta == null && !bSerializeOnly) || Property.GetCustomAttribute<HideAttribute>() != null)
            {
                continue;
            }

            ScriptType Resolved = ResolveType(Property.PropertyType, Depth + 1, Visiting, Property.GetCustomAttribute<InstancedAttribute>() != null);
            if (Resolved.Kind == EPropertyType.None)
            {
                continue;
            }

            // A get-only property USED to mean "not editable, so not a property". That is still true for a
            // value, but not for a container: a script's container property is a get-only VIEW over storage
            // native owns, so assigning it is meaningless while its contents are fully editable. Requiring a
            // setter dropped every such member from the schema, so no native property was appended and the
            // view had nothing to point at.
            bool bNativeOwnedView = !Property.CanWrite;
            if (bNativeOwnedView && !IsNativeOwnedViewType(Property.PropertyType))
            {
                continue;
            }

            Members.Add(new ScriptProperty
            {
                Name = Meta?.Name ?? Property.Name,
                Type = Resolved,
                Meta = Meta,
                Hidden = bSerializeOnly,
                Aliases = GatherAliases(Property),
                SkipHotReload = bClassSkip || Property.GetCustomAttribute<SkipHotReloadAttribute>() != null,
                Get = Property.GetValue,
                // Never Property.SetValue for a view: there is no setter to call, and reaching for one throws.
                Set = bNativeOwnedView ? (Instance, Value) => { } : Property.SetValue,
                IsNativeOwnedView = bNativeOwnedView,
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
        // The list VIEWS, which is what a [Property] container on a script type is declared as (there is one
        // copy of the value and native owns it, so a List<T> would be a lie). Each is shaped identically to
        // a List<T> on the wire; ScriptPropertyViews knows which they are and what their element is.
        if (ScriptPropertyViews.TryGetElementType(Type, out ElementType))
        {
            return true;
        }
        ElementType = null;
        return false;
    }

    private static bool TryGetMapTypes(Type Type, out Type? KeyType, out Type? ValueType)
    {
        // A managed Dictionary<K,V> -- a data struct's own member, copied across the wire.
        if (Type.IsGenericType && Type.GetGenericTypeDefinition() == typeof(Dictionary<,>))
        {
            Type[] Args = Type.GetGenericArguments();
            KeyType = Args[0];
            ValueType = Args[1];
            return true;
        }
        // THashMap<K,V>: a VIEW over the native map, shaped identically on the wire.
        return ScriptPropertyViews.TryGetKeyValue(Type, out KeyType, out ValueType);
    }
}

// Cached, immutable-after-Build description of one script type; Create() uses Activator deliberately, since a compiled factory would pin this collectible ALC's ctor and block hot-reload unload.
internal sealed class TypeDescription
{
    public Type Type { get; }
    public IReadOnlyList<ScriptProperty> Properties { get; private set; } = Array.Empty<ScriptProperty>();
    public IReadOnlyList<ScriptButton> Buttons { get; private set; } = Array.Empty<ScriptButton>();
    public IReadOnlyList<ScriptFunction> Functions { get; private set; } = Array.Empty<ScriptFunction>();
    private IReadOnlyList<ScriptProperty> InputBindings = Array.Empty<ScriptProperty>();
    public bool HasInputBindings { get; private set; }

    public TypeDescription(Type Type)
    {
        this.Type = Type;
    }

    public void Build(TypeLibrary Library)
    {
        Properties = Library.BuildMembers(Type, 0, new HashSet<Type>());
        Buttons = ComputeButtons(Type);
        Functions = ComputeFunctions(Type, Library);
        InputBindings = ComputeInputBindings(Properties);
        HasInputBindings = InputBindings.Count > 0;
    }

    // The [ScriptFunction] methods, described the same way a property is: a parameter is a field of the call
    // frame, so it goes through the same type resolver and needs no description of its own.
    private static IReadOnlyList<ScriptFunction> ComputeFunctions(Type Type, TypeLibrary Library)
    {
        List<ScriptFunction>? Found = null;
        HashSet<string>? Overloaded = OverloadedFunctionNames(Type);

        foreach (MethodInfo Method in Type.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
        {
            if (Method.GetCustomAttribute<ScriptFunctionAttribute>() == null)
            {
                continue;
            }

            if (Method.IsGenericMethod)
            {
                Debug.LogError($"[ScriptFunction] {Type.Name}.{Method.Name} is generic, which has no single call frame; it is not reflected.");
                continue;
            }

            if (Overloaded != null && Overloaded.Contains(Method.Name))
            {
                continue;
            }

            List<ScriptProperty> Params = new();
            foreach (ParameterInfo Parameter in Method.GetParameters())
            {
                // A frame slot holds the value, so a by-ref parameter is described as the type behind it.
                Type Declared = Parameter.ParameterType;
                if (Declared.IsByRef)
                {
                    Declared = Declared.GetElementType()!;
                }

                Params.Add(new ScriptProperty
                {
                    Name = Parameter.Name ?? $"Arg{Params.Count}",
                    Type = Library.ResolveType(Declared, 0, new HashSet<Type>()),
                    ParamFlags = FrameMarshal.DirectionOf(Parameter),
                });
            }

            if (ScriptAsync.WhyUnsupported(Method.ReturnType) is string Why)
            {
                Debug.LogError($"Script function '{Method.Name}' returns {Method.ReturnType.Name}, and {Why}");
                continue;
            }

            int ReturnIndex = -1;
            if (Method.ReturnType != typeof(void))
            {
                ReturnIndex = Params.Count;
                Params.Add(new ScriptProperty
                {
                    Name = "ReturnValue",
                    // An async function hands back a token native polls, since a frame cannot wait for it.
                    Type = ScriptAsync.IsFireAndForget(Method.ReturnType)
                        ? Library.ResolveType(typeof(ulong), 0, new HashSet<Type>())
                        : Library.ResolveType(Method.ReturnType, 0, new HashSet<Type>()),
                });
            }

            Found ??= new List<ScriptFunction>();
            Found.Add(new ScriptFunction(Method.Name, Params, ReturnIndex));
        }

        return (IReadOnlyList<ScriptFunction>?)Found ?? Array.Empty<ScriptFunction>();
    }

    // Both sides key a reflected function by name, so an overload has nothing to tell it apart from its twin.
    private static HashSet<string>? OverloadedFunctionNames(Type Type)
    {
        Dictionary<string, int> Declared = new(StringComparer.Ordinal);

        foreach (MethodInfo Method in Type.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
        {
            if (Method.GetCustomAttribute<ScriptFunctionAttribute>() == null)
            {
                continue;
            }

            Declared.TryGetValue(Method.Name, out int Seen);
            Declared[Method.Name] = Seen + 1;
        }

        HashSet<string>? Overloaded = null;
        foreach (KeyValuePair<string, int> Pair in Declared)
        {
            if (Pair.Value > 1)
            {
                Debug.LogError($"[ScriptFunction] {Type.Name}.{Pair.Key} is declared {Pair.Value} times. A reflected function is looked up by name from native and from the dispatcher, so an overload cannot be told apart and none of them is reflected. Give each one its own name.");
                (Overloaded ??= new HashSet<string>(StringComparer.Ordinal)).Add(Pair.Key);
            }
        }

        return Overloaded;
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
    /// SInputAction field is enough on its own. Idempotent, so a script
    /// that also calls EnableInput() is unaffected.</summary>
    public void EnsureInputComponent(EntityScript Script)
    {
        if (InputBindings.Count > 0)
        {
            Script.Registry.Emplace<Lumina.SInputComponent>(Script.Entity);
        }
    }

    /// <summary>Hands one action's state to each of the instance's bindings that listens to it.</summary>
    public void DispatchAction(EntityScript Script, Lumina.FName Action, in Lumina.FInputActionState State)
    {
        foreach (ScriptProperty Property in InputBindings)
        {
            // A binding the script nulled out is skipped rather than recreated: the field is the script's
            // to own, and silently handing it a new object would lose whatever it meant by clearing it.
            if (Property.Get(Script) is SInputBinding Binding && Binding.Listens(Action))
            {
                Binding.Apply(State);
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
