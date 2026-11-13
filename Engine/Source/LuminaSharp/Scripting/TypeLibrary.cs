using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using Lumina;
using LuminaSharp.ScriptProperties;

namespace LuminaSharp;

// The managed reflection registry for one loaded script generation, resolving any type to a cached TypeDescription. Rebuilt on every load.
internal sealed class TypeLibrary
{
    // C# subclasses of reflected native classes, keyed by full name, the host minting a CClass per one.
    private readonly Dictionary<string, Type> Scriptables = new();
    // Every [Reflect] struct, keyed by the simple name an asset stores; the host mints a CScriptStruct per one.
    private readonly Dictionary<string, ScriptStructEntry> ScriptStructs = new();
    // Every [Reflect] enum, keyed by its simple name; the host mints a CScriptEnum per one.
    private readonly Dictionary<string, Type> Enums = new();
    private readonly Dictionary<Type, TypeDescription> ByType = new();
    // Prior full type names to current full name, so renamed script references resolve.
    private readonly Dictionary<string, string> ScriptAliases = new();
    // Every loaded script type, the universe an instanced field's candidate concrete types are drawn from.
    private readonly List<Type> AllTypes;

    public TypeLibrary(IEnumerable<Type> Types)
    {
        // Proves the view table still matches the classifier the rewriter emitted accessors from, failures it catches being otherwise silent.
        ScriptPropertyViews.Validate();

        StateTreeNodes = new StateTreeNodeInvokers(this);
        AllTypes = new List<Type>(Types);
        foreach (Type Type in AllTypes)
        {
            if (Type.FullName == null || Type.IsInterface || Type.ContainsGenericParameters)
            {
                continue;
            }
            if (!IsScriptableSubclass(Type))
            {
                continue;
            }
            // Abstract ones too, since a subclass is minted under its C# parent and needs that class to exist.
            if (!Type.IsDefined(typeof(ReflectAttribute), inherit: false))
            {
                // An unreflected abstract class is a plain C# base, whose members the reflected classes below it carry.
                if (Type.IsAbstract)
                {
                    continue;
                }
                Native.Log(ELogLevel.Warn,
                    $"'{Type.FullName}' derives from a native script class without [Reflect], so the engine does not see it. "
                    + "Add [Reflect] to register it.");
                continue;
            }
            Scriptables[NativeTypeName.Of(Type)] = Type;
        }

        // Its own pass, since a struct or enum is published for carrying [Reflect] whether or not anything references it.
        List<Type> ReflectedValueTypes = new();
        foreach (Type Type in AllTypes)
        {
            if (!Type.IsValueType || Type.IsPrimitive || Type.ContainsGenericParameters || Type.IsDefined(typeof(NativeTypeAttribute), inherit: false))
            {
                continue;
            }

            bool bReflected = Type.IsDefined(typeof(ReflectAttribute), inherit: false);
            Type? NativeBase = Type.IsEnum ? null : NativeStructBaseOf(Type);
            if (!bReflected)
            {
                if (NativeBase != null || IsComponentStruct(Type))
                {
                    Native.Log(ELogLevel.Warn,
                        $"'{Type.FullName}' lives in native memory as a {(NativeBase != null ? "native struct's extension" : "component")} but has no [Reflect], "
                        + "so the engine does not see it. Add [Reflect] to register it.");
                }
                continue;
            }

            string StableId = NativeTypeName.Of(Type);
            if (ScriptStructs.ContainsKey(StableId) || Enums.ContainsKey(StableId))
            {
                Native.Log(ELogLevel.Warn,
                    $"Reflected type name '{StableId}' is claimed by more than one C# type, '{Type.FullName}' among them; "
                    + "ignoring the latter. Rename one, they share one name space with every native type.");
                continue;
            }

            ReflectedValueTypes.Add(Type);
            if (Type.IsEnum)
            {
                Enums[StableId] = Type;
                continue;
            }
            ScriptStructs[StableId] = new ScriptStructEntry(Describe(Type), NativeBase != null ? NativeTypeName.Of(NativeBase) : "");
        }

        // Built once every current name is known, so an alias never shadows a live type.
        foreach (Type Renamed in Scriptables.Values.Concat(ReflectedValueTypes))
        {
            string Current = NativeTypeName.Of(Renamed);
            // Not inherited, since a subclass is not the class that used to have the name.
            foreach (AliasAttribute Alias in Renamed.GetCustomAttributes<AliasAttribute>(inherit: false))
            {
                if (string.IsNullOrEmpty(Alias.Name) || Scriptables.ContainsKey(Alias.Name) || ScriptStructs.ContainsKey(Alias.Name) || Enums.ContainsKey(Alias.Name))
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

    // Prior type names to current ones from [Alias], which native mirrors so saved scenes and live instances follow a rename.
    public IEnumerable<KeyValuePair<string, string>> TypeAliases => ScriptAliases;

    public IReadOnlyCollection<string> ScriptableTypeNames => Scriptables.Keys;

    public IEnumerable<Type> ScriptableTypes => Scriptables.Values;

    // Every reflected struct as (StableId, description, native base name), each after the structs it holds by value.
    public IEnumerable<KeyValuePair<string, ScriptStructEntry>> ScriptStructTypes
    {
        get
        {
            var Ordered = new List<KeyValuePair<string, ScriptStructEntry>>(ScriptStructs.Count);
            var Placed = new HashSet<string>();
            void Place(string Name, ScriptStructEntry Entry)
            {
                if (!Placed.Add(Name))
                {
                    return;
                }
                foreach (ScriptProperty Field in Entry.Description.Properties)
                {
                    foreach (string Held in HeldStructNames(Field.Type))
                    {
                        if (ScriptStructs.TryGetValue(Held, out ScriptStructEntry Dependency))
                        {
                            Place(Held, Dependency);
                        }
                    }
                }
                Ordered.Add(new KeyValuePair<string, ScriptStructEntry>(Name, Entry));
            }
            foreach (KeyValuePair<string, ScriptStructEntry> Pair in ScriptStructs)
            {
                Place(Pair.Key, Pair.Value);
            }
            return Ordered;
        }
    }

    // Every reflected enum as (name, type).
    public IEnumerable<KeyValuePair<string, Type>> EnumTypes => Enums;

    // Builds the handle for a script struct named at runtime, so a caller can reach it through an interface it implements.
    private readonly System.Collections.Concurrent.ConcurrentDictionary<string, Func<nint, object>?> StructViews = new();

    // The invokers state trees run this generation's C# nodes through.
    internal readonly StateTreeNodeInvokers StateTreeNodes;

    public object? ViewScriptStruct(string StableId, nint Address)
    {
        if (Address == 0)
        {
            return null;
        }
        Func<nint, object>? View = StructViews.GetOrAdd(StableId, Name =>
        {
            // A component struct is reached by ref rather than through a handle, so it has no view to build.
            if (!ScriptStructs.TryGetValue(Name, out ScriptStructEntry Entry) || !typeof(INativeStruct).IsAssignableFrom(Entry.Description.Type))
            {
                return null;
            }
            MethodInfo Box = typeof(TypeLibrary).GetMethod(nameof(BoxView), BindingFlags.NonPublic | BindingFlags.Static)!
                .MakeGenericMethod(Entry.Description.Type);
            return Box.CreateDelegate<Func<nint, object>>();
        });
        return View?.Invoke(Address);
    }

    private static object BoxView<T>(nint Address) where T : struct, INativeStruct => NativeStructs.FromAddress<T>(Address);

    private static bool IsEngineNamespace(string? Space) => Space != null && (Space.StartsWith("System") || Space.StartsWith("LuminaSharp"));

    // The user interfaces a script struct implements, which an instanced property's Implements filter matches against.
    public static string ImplementedInterfaces(Type Type)
    {
        IEnumerable<string> Names = Type.GetInterfaces()
            .Where(Interface => Interface.FullName != null && !IsEngineNamespace(Interface.Namespace))
            .Select(Interface => Interface.FullName!);
        return string.Join(";", Names);
    }

    private static IEnumerable<string> HeldStructNames(ScriptType Type)
    {
        if (Type.Kind is EPropertyType.Struct or EPropertyType.ComponentRef && Type.NativeName != null)
        {
            yield return Type.NativeName;
        }
        foreach (ScriptType? Inner in new[] { Type.Element, Type.KeyType, Type.ValueType })
        {
            if (Inner != null)
            {
                foreach (string Name in HeldStructNames(Inner))
                {
                    yield return Name;
                }
            }
        }
    }

    // The description for a script struct by StableId, or null if unknown.
    public TypeDescription? GetScriptStruct(string StableId)
    {
        return ScriptStructs.TryGetValue(StableId, out ScriptStructEntry Entry) ? Entry.Description : null;
    }

    public Type? GetScriptable(string FullName) => Scriptables.TryGetValue(FullName, out Type? Type) ? Type : null;

    // A type that views native storage, so a get-only member of it is still a real property.
    internal static bool IsNativeOwnedViewType(Type Type) => ScriptPropertyViews.IsView(Type) || IsInstancedStructView(Type);

    private static bool IsInstancedStructView(Type Type)
        => Type == typeof(FInstancedStruct) || (Type.IsGenericType && Type.GetGenericTypeDefinition() == typeof(TInstancedStruct<>));

    // The nearest C# class above Type that is minted too, which is what Type is minted under, or null below a native base.
    public Type? MintedParentOf(Type Type)
    {
        for (Type? Base = Type.BaseType; Base != null; Base = Base.BaseType)
        {
            if (IsMinted(Base))
            {
                return Base;
            }
        }
        return null;
    }

    private bool IsMinted(Type Type) => Scriptables.TryGetValue(NativeTypeName.Of(Type), out Type? Minted) && Minted == Type;

    // Type and the engine C# classes above it that native never mints, whose declarations the minted class carries for them.
    public IEnumerable<Type> ChainUpToMintedParent(Type Type)
    {
        yield return Type;
        for (Type? Base = Type.BaseType; Base != null && !IsMinted(Base) && !Base.IsDefined(typeof(NativeTypeAttribute), inherit: false);
             Base = Base.BaseType)
        {
            yield return Base;
        }
    }

    // The native struct a C# struct names through IExtends, or null when it extends none.
    public static Type? NativeStructBaseOf(Type Type)
    {
        if (!Type.IsValueType || Type.IsDefined(typeof(NativeTypeAttribute), inherit: false))
        {
            return null;
        }
        foreach (Type Interface in Type.GetInterfaces())
        {
            if (Interface.IsGenericType && Interface.GetGenericTypeDefinition() == typeof(IExtends<>))
            {
                return Interface.GetGenericArguments()[0];
            }
        }
        return null;
    }

    // A C# struct or enum is a reflected type only through [Reflect], as a C++ one is only through REFLECT.
    public static bool IsReflectedType(Type Type) =>
        Type.IsDefined(typeof(ReflectAttribute), inherit: false) || Type.IsDefined(typeof(NativeTypeAttribute), inherit: false);

    // A struct living in native memory, as a root, an extension of a native struct or a component, joins reflection only through [Reflect].
    public static bool IsNativeBackedStruct(Type Type)
    {
        return Type.IsDefined(typeof(ReflectAttribute), inherit: false)
            && (NativeStructBaseOf(Type) != null || IsComponentStruct(Type) || ImplementsSelf(Type, typeof(INativeStruct<>)));
    }

    // A C# struct the ECS stores per entity, which it declares as REFLECT(Component) does in C++ by implementing IComponent.
    public static bool IsComponentStruct(Type Type) => ImplementsSelf(Type, typeof(IComponent<>));

    private static bool ImplementsSelf(Type Type, Type SelfInterface)
    {
        if (!Type.IsValueType || Type.IsDefined(typeof(NativeTypeAttribute), inherit: false))
        {
            return false;
        }
        foreach (Type Interface in Type.GetInterfaces())
        {
            if (Interface.IsGenericType && Interface.GetGenericTypeDefinition() == SelfInterface && Interface.GetGenericArguments()[0] == Type)
            {
                return true;
            }
        }
        return false;
    }

    // A generated wrapper of a native class, a game module's included, names that class and is never minted.
    private static bool IsScriptableSubclass(Type Type)
    {
        return Type.GetCustomAttribute<NativeTypeAttribute>(inherit: false) == null && NativeClassBaseOf(Type) != null;
    }

    // The nearest generated wrapper above Type, which is the reflected native class a C# class extends.
    public static Type? NativeClassBaseOf(Type Type)
    {
        for (Type? Base = Type.BaseType; Base != null && Base != typeof(NativeObject); Base = Base.BaseType)
        {
            if (Base.IsDefined(typeof(NativeTypeAttribute), inherit: false) && typeof(NativeObject).IsAssignableFrom(Base))
            {
                return Base;
            }
        }
        return null;
    }

    // Locked whole, since an entry is published before it is built and a parallel system must not read it half done.
    public TypeDescription Describe(Type Type)
    {
        lock (ByType)
        {
            if (ByType.TryGetValue(Type, out TypeDescription? Cached))
            {
                return Cached;
            }

            TypeDescription Description = new(Type);
            ByType[Type] = Description; // Inserted before its members build, so a self or cyclic reference resolves.
            Description.Build(this);
            return Description;
        }
    }

    // The reflected shape of a CLR type, which names any struct, enum or class rather than describing it, or None when it has none.
    public ScriptType ResolveType(Type Type)
    {
        if (Type == typeof(bool))
        {
            return new ScriptType { Kind = EPropertyType.Bool, Clr = Type };
        }
        // An interned name round-trips as its own kind, C# reading the id in place.
        if (Type == typeof(FName))
        {
            return new ScriptType { Kind = EPropertyType.Name, Clr = Type };
        }
        // Both string spellings are one native FString, and only FString can be a container element.
        if (Type == typeof(string) || Type == typeof(FString))
        {
            return new ScriptType { Kind = EPropertyType.String, Clr = Type };
        }
        if (Type == typeof(FEntity))
        {
            return new ScriptType { Kind = EPropertyType.Entity, Clr = Type };
        }
        // An input binding is stored as its action name, tagged so the editor draws the action picker.
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

        // A field names its enum, which is reflected once like any other type, as REFLECT requires of a C++ one.
        if (Type.IsEnum)
        {
            EPropertyType Underlying = MapNumeric(Enum.GetUnderlyingType(Type));
            if (Underlying == EPropertyType.None || !IsReflectedType(Type))
            {
                return new ScriptType { Kind = EPropertyType.None, Clr = Type };
            }
            return new ScriptType { Kind = EPropertyType.Enum, Clr = Type, EnumName = NativeTypeName.Of(Type), EnumUnderlying = Underlying };
        }

        // An asset reference is a path, drawn as an asset picker filtered to the target class.
        if (Type == typeof(FSoftObjectPath))
        {
            return new ScriptType { Kind = EPropertyType.SoftObject, Clr = Type, TargetClass = "" };
        }
        // A nullable is an optional over its payload, matching the native TOptional a C++ one reflects as.
        if (Nullable.GetUnderlyingType(Type) is Type Payload)
        {
            ScriptType Inner = ResolveType(Payload);
            if (Inner.Kind == EPropertyType.None)
            {
                return new ScriptType { Kind = EPropertyType.None, Clr = Type };
            }
            return new ScriptType { Kind = EPropertyType.Optional, Clr = Type, Element = Inner };
        }

        if (Type.IsGenericType)
        {
            Type Definition = Type.GetGenericTypeDefinition();
            // A soft reference is a path resolved on demand, while a strong one is a live pointer that keeps its target alive.
            if (Definition == typeof(TSoftObjectPtr<>))
            {
                return new ScriptType { Kind = EPropertyType.SoftObject, Clr = Type, TargetClass = NativeTypeName.Of(Type.GetGenericArguments()[0]) };
            }
            if (Definition == typeof(TStrongObjectPtr<>))
            {
                return new ScriptType { Kind = EPropertyType.Object, Clr = Type, TargetClass = NativeTypeName.Of(Type.GetGenericArguments()[0]) };
            }
        }

        if (IsInstancedStructView(Type))
        {
            string? BaseName = Type.IsGenericType ? ResolveType(Type.GetGenericArguments()[0]).NativeName : null;
            return new ScriptType { Kind = EPropertyType.InstancedStruct, Clr = Type, BaseName = BaseName };
        }

        // A wrapper around a native CObject is a hard reference, stored as an object property.
        if (typeof(NativeObject).IsAssignableFrom(Type))
        {
            return new ScriptType { Kind = EPropertyType.Object, Clr = Type, TargetClass = NativeTypeName.Of(Type) };
        }

        // A List<T> or T[].
        if (TryGetElementType(Type, out Type? ElementType))
        {
            ScriptType Element = ResolveType(ElementType!);
            if (Element.Kind == EPropertyType.None)
            {
                return new ScriptType { Kind = EPropertyType.None, Clr = Type };
            }
            return new ScriptType { Kind = EPropertyType.Vector, Clr = Type, Element = Element };
        }

        // A Dictionary<K,V> is a reflected map, dropped if either key or value has no reflected type.
        if (TryGetMapTypes(Type, out Type? KeyClr, out Type? ValueClr))
        {
            ScriptType Key = ResolveType(KeyClr!);
            ScriptType Value = ResolveType(ValueClr!);
            if (Key.Kind == EPropertyType.None || Value.Kind == EPropertyType.None)
            {
                return new ScriptType { Kind = EPropertyType.None, Clr = Type };
            }
            return new ScriptType { Kind = EPropertyType.Map, Clr = Type, KeyType = Key, ValueType = Value };
        }

        // A native struct or a [Reflect] C# one is a reflected type of its own, which the field names.
        string? StructName = Type.GetCustomAttribute<NativeLayoutAttribute>()?.NativeType ?? (IsReflectedType(Type) ? NativeTypeName.Of(Type) : null);
        if (Type.IsValueType && !Type.IsPrimitive && !Type.IsEnum && StructName != null)
        {
            return new ScriptType { Kind = EPropertyType.Struct, Clr = Type, NativeName = StructName };
        }

        return new ScriptType { Kind = EPropertyType.None, Clr = Type };
    }

    internal static EPropertyType MapNumeric(Type Type)
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

    // Every member ScriptMemberRules stores, fields and properties alike, under its own name.
    internal List<ScriptProperty> BuildMembers(Type Type)
    {
        var Members = new List<ScriptProperty>();
        const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;
        bool bClassSkip = Type.GetCustomAttribute<SkipHotReloadAttribute>() != null;
        bool bNativeObject = typeof(NativeObject).IsAssignableFrom(Type) || IsNativeBackedStruct(Type);

        foreach (MemberInfo Member in Type.GetMembers(Flags))
        {
            Type MemberType;
            EScriptMemberKind Kind;
            if (Member is FieldInfo Field)
            {
                MemberType = Field.FieldType;
                Kind = EScriptMemberKind.Field;
            }
            else if (Member is PropertyInfo Property && Property.CanRead && Property.GetIndexParameters().Length == 0)
            {
                MemberType = Property.PropertyType;

                // The rewriter turns a native struct field on a class into a ref readonly view of the owner's storage.
                bool bStructView = MemberType.IsByRef;
                if (bStructView)
                {
                    MemberType = MemberType.GetElementType()!;
                }

                // A get-only container is a view over storage native owns, and a component reference is filled by native, so both are stored with no setter.
                bool bComponentRef = Property.GetCustomAttribute<PropertyAttribute>() is { Component: not EComponentBind.None };
                Kind = Property.CanWrite || bStructView || bComponentRef || IsNativeOwnedViewType(MemberType) ? EScriptMemberKind.WritableProperty : EScriptMemberKind.ReadOnlyProperty;
            }
            else
            {
                continue;
            }

            FScriptMemberRule Rule = RuleFor(Member, Kind, bNativeObject);
            if (!Rule.bStored)
            {
                continue;
            }

            PropertyAttribute? Meta = Member.GetCustomAttribute<PropertyAttribute>();
            ScriptType Resolved = Meta != null && Meta.Component != EComponentBind.None ? ResolveComponentRef(MemberType) : ResolveType(MemberType);
            if (Resolved.Kind == EPropertyType.None)
            {
                continue;
            }

            Members.Add(new ScriptProperty
            {
                Name = Member.Name,
                DeclaringType = StorageOwnerOf(Member, Kind, bNativeObject),
                Type = Resolved,
                Meta = Meta,
                Hidden = !Rule.bVisible,
                Aliases = GatherAliases(Member, Meta),
                SkipHotReload = bClassSkip || Member.GetCustomAttribute<SkipHotReloadAttribute>() != null,
                ExtraFlags = (EPropertyFlags)Rule.Flags,
                NetRate = Meta?.Rate ?? 0.0f,
                ExtraMeta = ExtraMetaOf(Member, Meta),
                Get = Member is FieldInfo F ? F.GetValue : ((PropertyInfo)Member).GetValue,
            });
        }

        return Members;
    }

    // An override reads and writes the storage of the stored property it overrides, so it counts as declared where that storage is.
    private static Type? StorageOwnerOf(MemberInfo Member, EScriptMemberKind Kind, bool bNativeObject)
    {
        Type? Owner = Member.DeclaringType;
        if (Member is not PropertyInfo Property || Property.GetMethod is not { } Getter || Getter.GetBaseDefinition().DeclaringType == Owner)
        {
            return Owner;
        }

        const BindingFlags Declared = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
        for (Type? Base = Owner?.BaseType; Base != null; Base = Base.BaseType)
        {
            // A class between the two can leave the property alone, so only a redeclaring one is considered.
            if (Base.GetProperty(Property.Name, Declared) is not { GetMethod: { IsVirtual: true } BaseGetter } Overridden)
            {
                continue;
            }
            if (!BaseGetter.IsAbstract && RuleFor(Overridden, Kind, bNativeObject).bStored)
            {
                Owner = Base;
            }
            if (BaseGetter.GetBaseDefinition().DeclaringType == Base)
            {
                break;
            }
        }
        return Owner;
    }

    // A reference names the component it binds, the T of the TComponentRef<T> it is declared as.
    private ScriptType ResolveComponentRef(Type Type)
    {
        Type? Component = Type.IsGenericType && Type.GetGenericTypeDefinition() == typeof(TComponentRef<>) ? Type.GetGenericArguments()[0] : null;
        ScriptType? Value = Component != null ? ResolveType(Component) : null;
        return Value is { Kind: EPropertyType.Struct }
            ? new ScriptType { Kind = EPropertyType.ComponentRef, Clr = Value.Clr, NativeName = Value.NativeName }
            : new ScriptType { Kind = EPropertyType.None, Clr = Type };
    }

    internal static FScriptMemberRule RuleFor(MemberInfo Member, EScriptMemberKind Kind, bool bOwnerIsNativeObject)
    {
        PropertyAttribute? Property = Member.GetCustomAttribute<PropertyAttribute>(inherit: true);
        return ScriptMemberRules.Classify(Property != null ? Property.Has : null, Kind, bOwnerIsNativeObject);
    }

    // Prior names a saved value may carry, [Alias] and the DisplayName that used to be the stored name.
    private static IReadOnlyList<string>? GatherAliases(MemberInfo Member, PropertyAttribute? Meta)
    {
        List<string>? Result = null;
        if (!string.IsNullOrEmpty(Meta?.DisplayName) && Meta.DisplayName != Member.Name)
        {
            (Result ??= new List<string>()).Add(Meta.DisplayName);
        }
        foreach (AliasAttribute Alias in Member.GetCustomAttributes<AliasAttribute>(inherit: false))
        {
            if (!string.IsNullOrEmpty(Alias.Name))
            {
                (Result ??= new List<string>()).Add(Alias.Name);
            }
        }
        return Result;
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
        // A [Property] container is declared as a view, since native owns the one copy of the value.
        if (ScriptPropertyViews.TryGetElementType(Type, out ElementType))
        {
            return true;
        }
        ElementType = null;
        return false;
    }

    // The same keys and values the C++ PROPERTY(Sync = ..., Change = ..., AssetType = ..., Bundle = ..., Group = ...) gives a field.
    private static List<KeyValuePair<string, string>>? ExtraMetaOf(MemberInfo Member, PropertyAttribute? Meta)
    {
        var Result = new List<KeyValuePair<string, string>>();
        void AddIfSet(string Key, string? Value)
        {
            if (!string.IsNullOrEmpty(Value))
            {
                Result.Add(new(Key, Value));
            }
        }
        AddIfSet(nameof(PropertyAttribute.DisplayName), Meta?.DisplayName);
        AddIfSet(nameof(PropertyAttribute.AssetType), Meta?.AssetType);
        AddIfSet(nameof(PropertyAttribute.Bundle), Meta?.Bundle);
        AddIfSet(nameof(PropertyAttribute.Group), Meta?.Group);
        AddIfSet(nameof(PropertyAttribute.Implements), Meta?.Implements?.FullName);
        if (Meta != null && Meta.Component != EComponentBind.None)
        {
            Result.Add(new(nameof(PropertyAttribute.Component), Meta.Component.ToString()));
        }
        if (Meta == null)
        {
            return Result.Count > 0 ? Result : null;
        }
        if (Meta.Has(ESpecifier.Bind))
        {
            Result.Add(new(nameof(ESpecifier.Bind), Meta.Bind ?? ""));
        }
        AddIfSet(nameof(PropertyAttribute.Change), Meta.Change);
        if (Meta.Has(ESpecifier.Sync))
        {
            Result.Add(new(nameof(ESpecifier.Sync), (Meta.Sync & Lumina.ESyncFlags.FromOwner) != 0 ? "FromOwner" : ""));
            if (Meta.Quantize > 0.0)
            {
                Result.Add(new(nameof(PropertyAttribute.Quantize), Meta.Quantize.ToString("R", System.Globalization.CultureInfo.InvariantCulture)));
            }
            AddIfSet(nameof(PropertyAttribute.Validate), Meta.Validate);
        }
        return Result.Count > 0 ? Result : null;
    }

    private static bool TryGetMapTypes(Type Type, out Type? KeyType, out Type? ValueType)
    {
        // A managed Dictionary is a data struct's own member, copied across the wire.
        if (Type.IsGenericType && Type.GetGenericTypeDefinition() == typeof(Dictionary<,>))
        {
            Type[] Args = Type.GetGenericArguments();
            KeyType = Args[0];
            ValueType = Args[1];
            return true;
        }
        // A THashMap is a view over the native map, shaped the same on the wire.
        return ScriptPropertyViews.TryGetKeyValue(Type, out KeyType, out ValueType);
    }
}

// Cached, immutable after Build, description of one script type.
internal sealed class TypeDescription
{
    public Type Type { get; }
    public IReadOnlyList<ScriptProperty> Properties { get; private set; } = Array.Empty<ScriptProperty>();
    public IReadOnlyList<ScriptFunction> Functions { get; private set; } = Array.Empty<ScriptFunction>();
    private IReadOnlyList<ScriptProperty> InputBindings = Array.Empty<ScriptProperty>();
    public bool HasInputBindings { get; private set; }

    public TypeDescription(Type Type)
    {
        this.Type = Type;
    }

    public void Build(TypeLibrary Library)
    {
        Properties = Library.BuildMembers(Type);
        Functions = ComputeFunctions(Type, Library);
        InputBindings = ComputeInputBindings(Properties);
        HasInputBindings = InputBindings.Count > 0;
    }

    // EFunctionFlags bits, mirroring Function.h.
    private const uint RpcBroadcastFlag = 1u << 7;
    private const uint RpcHostFlag = 1u << 8;
    private const uint RpcOwnerFlag = 1u << 9;
    private const uint NetUnreliableFlag = 1u << 10;
    private const uint NetOwnerOnlyFlag = 1u << 11;

    // A [Function], an [Rpc] method, or a method a Sync or Bind field names, each of which native calls through a reflected frame.
    private static bool IsReflectedMethod(MethodInfo Method, HashSet<string> NamedBySync)
    {
        return Method.GetCustomAttribute<FunctionAttribute>() != null
            || Method.GetCustomAttribute<Rpc.RpcAttribute>() != null
            || IsUIBound(Method)
            || NamedBySync.Contains(Method.Name);
    }

    // A [Function(Bind)] method, or the getter of a get-only [Property(Bind)] property, which a UI document reads as a computed value.
    private static bool IsUIBound(MethodInfo Method)
    {
        if (Method.GetCustomAttribute<FunctionAttribute>()?.Has(ESpecifier.Bind) == true)
        {
            return true;
        }
        if (!Method.IsSpecialName || !Method.Name.StartsWith("get_", StringComparison.Ordinal) || Method.DeclaringType == null)
        {
            return false;
        }
        PropertyInfo? Property = Method.DeclaringType.GetProperty(Method.Name.Substring(4),
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly);
        return Property != null && Property.SetMethod == null && Property.GetCustomAttribute<PropertyAttribute>()?.Has(ESpecifier.Bind) == true
            && !TypeLibrary.IsNativeOwnedViewType(Property.PropertyType);
    }

    private static HashSet<string> MethodsNamedBySync(Type Type)
    {
        var Result = new HashSet<string>(StringComparer.Ordinal);
        const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        foreach (MemberInfo Member in Type.GetFields(Flags).Cast<MemberInfo>().Concat(Type.GetProperties(Flags)))
        {
            PropertyAttribute? Property = Member.GetCustomAttribute<PropertyAttribute>();
            if (Property == null || (!Property.Has(ESpecifier.Sync) && !Property.Has(ESpecifier.Bind)))
            {
                continue;
            }
            if (Property.Has(ESpecifier.Sync) && Property.Validate != null)
            {
                Result.Add(Property.Validate);
            }
            if (Property.Change != null)
            {
                Result.Add(Property.Change);
            }
        }
        return Result;
    }

    // EFunctionFlags::UIBind, mirroring Function.h.
    private const uint UIBindFlag = 1u << 12;

    // Button and ToolTip are ordinary function metadata, the same keys FUNCTION(Button = ..., ToolTip = ...) writes.
    private static IReadOnlyList<KeyValuePair<string, string>>? FunctionMetaOf(MethodInfo Method)
    {
        if (Method.GetCustomAttribute<FunctionAttribute>() is not FunctionAttribute Function)
        {
            return null;
        }
        var Meta = new List<KeyValuePair<string, string>>();
        if (Function.Has(ESpecifier.Button))
        {
            if (Method.GetParameters().Length != 0)
            {
                Debug.LogWarning($"[Function(Button)] {Method.DeclaringType?.Name}.{Method.Name} takes parameters, so the inspector cannot call it.");
            }
            Meta.Add(new(nameof(ESpecifier.Button), Function.Button ?? ""));
        }
        if (!string.IsNullOrEmpty(Function.ToolTip))
        {
            Meta.Add(new(nameof(FunctionAttribute.ToolTip), Function.ToolTip));
        }
        return Meta.Count > 0 ? Meta : null;
    }

    private static uint FunctionFlagsOf(MethodInfo Method)
    {
        if (Method.GetCustomAttribute<Rpc.RpcAttribute>() is not Rpc.RpcAttribute Declared)
        {
            return IsUIBound(Method) ? UIBindFlag : 0;
        }
        uint Flags = Declared switch
        {
            Rpc.HostAttribute => RpcHostFlag,
            Rpc.OwnerAttribute => RpcOwnerFlag,
            _ => RpcBroadcastFlag,
        };
        if ((Declared.Flags & Lumina.ENetFlags.Unreliable) != 0)
        {
            Flags |= NetUnreliableFlag;
        }
        if ((Declared.Flags & Lumina.ENetFlags.OwnerOnly) != 0)
        {
            Flags |= NetOwnerOnlyFlag;
        }
        return Flags;
    }

    // Described the same way a property is, since a parameter is a field of the call frame and needs no description of its own.
    private static IReadOnlyList<ScriptFunction> ComputeFunctions(Type Type, TypeLibrary Library)
    {
        List<ScriptFunction>? Found = null;
        HashSet<string> NamedBySync = MethodsNamedBySync(Type);
        HashSet<string>? Overloaded = OverloadedFunctionNames(Type, NamedBySync);

        foreach (MethodInfo Method in Type.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
        {
            if (!IsReflectedMethod(Method, NamedBySync))
            {
                continue;
            }

            if (Method.IsGenericMethod)
            {
                Debug.LogError($"[Function] {Type.Name}.{Method.Name} is generic, which has no single call frame; it is not reflected.");
                continue;
            }

            if (Overloaded != null && Overloaded.Contains(Method.Name))
            {
                continue;
            }

            List<ScriptProperty> Params = new();
            string? Unreflected = null;
            foreach (ParameterInfo Parameter in Method.GetParameters())
            {
                // A frame slot holds the value, so a by-ref parameter is described as the type behind it.
                Type Declared = Parameter.ParameterType;
                if (Declared.IsByRef)
                {
                    Declared = Declared.GetElementType()!;
                }

                ScriptType Resolved = Library.ResolveType(Declared);
                if (Resolved.Kind == EPropertyType.None)
                {
                    Unreflected ??= $"parameter '{Parameter.Name}' has type '{Declared.Name}', which is not a reflected type"
                                  + (Declared.IsEnum || Declared.IsValueType ? ". Mark it [Reflect]." : ".");
                }
                Params.Add(new ScriptProperty
                {
                    Name = Parameter.Name ?? $"Arg{Params.Count}",
                    Type = Resolved,
                    ParamFlags = FrameMarshal.DirectionOf(Parameter),
                });
            }
            if (Unreflected != null)
            {
                Debug.LogError($"Script function '{Type.Name}.{Method.Name}' is not reflected, because its {Unreflected}");
                continue;
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
                        ? Library.ResolveType(typeof(ulong))
                        : Library.ResolveType(Method.ReturnType),
                });
            }

            Found ??= new List<ScriptFunction>();
            Found.Add(new ScriptFunction(Method.Name, Params, ReturnIndex, FunctionFlagsOf(Method), FunctionMetaOf(Method)));
        }

        return (IReadOnlyList<ScriptFunction>?)Found ?? Array.Empty<ScriptFunction>();
    }

    // Both sides key a reflected function by name, so an overload has nothing to tell it apart from its twin.
    private static HashSet<string>? OverloadedFunctionNames(Type Type, HashSet<string> NamedBySync)
    {
        Dictionary<string, int> Declared = new(StringComparer.Ordinal);

        foreach (MethodInfo Method in Type.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
        {
            if (!IsReflectedMethod(Method, NamedBySync))
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
                Debug.LogError($"[Function] {Type.Name}.{Pair.Key} is declared {Pair.Value} times. A reflected function is looked up by name from native and from the dispatcher, so an overload cannot be told apart and none of them is reflected. Give each one its own name.");
                (Overloaded ??= new HashSet<string>(StringComparer.Ordinal)).Add(Pair.Key);
            }
        }

        return Overloaded;
    }

    // Gathered once per type, so the per-frame poll walks a list without reflection.
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

    // Gives a script declaring input bindings the component that feeds them, idempotent so EnableInput() still works beside it.
    public void EnsureInputComponent(Lumina.CEntityScript Script)
    {
        if (InputBindings.Count > 0)
        {
            Script.World.GetOrEmplaceComponent<Lumina.SInputComponent>(Script.Entity);
        }
    }

    // Hands one action's state to each of the instance's bindings that listens to it.
    public void DispatchAction(Lumina.CEntityScript Script, Lumina.FName Action, in Lumina.SInputActionState State)
    {
        foreach (ScriptProperty Property in InputBindings)
        {
            // A binding the script nulled out is skipped, since the field is the script's and clearing it meant something.
            if (Property.Get(Script) is SInputBinding Binding && Binding.Listens(Action))
            {
                Binding.Apply(State);
            }
        }
    }
}

// A discovered data type, its member description and the native struct it derives from.
internal readonly struct ScriptStructEntry
{
    public ScriptStructEntry(TypeDescription Description, string NativeBase)
    {
        this.Description = Description;
        this.NativeBase = NativeBase;
    }

    public TypeDescription Description { get; }

    // The native CStruct the minted type is given as its super.
    public string NativeBase { get; }
}
