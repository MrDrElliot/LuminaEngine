using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;

namespace LuminaSharp.ScriptProperties;

// Linked by source into both the analyzer and LuminaSharp so the two cannot disagree; keep it dependency-free beyond Roslyn.
internal static class ScriptPropertyClassifier
{
    public static FScriptPropertyClassification Classify(ITypeSymbol Type)
    {
        if (IsComponent(Type))
        {
            return FScriptPropertyClassification.Reject(
                $"'{Display(Type)}' is a component, which lives in its entity's storage rather than in a field. "
                + $"Hold a TComponentRef<{Display(Type)}> marked [Property(Component = ...)].");
        }
        if (Type.TypeKind == TypeKind.Enum)
        {
            return HasMirrorAttribute(Type) || IsReflected(Type)
                ? FScriptPropertyClassification.Of(EScriptAccess.Enum)
                : FScriptPropertyClassification.Reject(
                    $"'{Display(Type)}' is an enum declared in script, and native stores it by its reflected type. Mark it [Reflect].");
        }
        if (Type.SpecialType == SpecialType.System_String)
        {
            return FScriptPropertyClassification.Of(EScriptAccess.String);
        }
        if (Type is INamedTypeSymbol { Name: "FEntity" } EntityType
            && EntityType.ContainingNamespace?.ToDisplayString() == "LuminaSharp")
        {
            return FScriptPropertyClassification.Of(EScriptAccess.Blittable);
        }

        // Asset references are pointer-sized and would pass the unmanaged test, but native stores an FSoftObjectPath.
        if (IsSoftObjectReference(Type))
        {
            return FScriptPropertyClassification.Of(EScriptAccess.AssetPath);
        }
        if (DerivesFromNativeObject(Type))
        {
            return FScriptPropertyClassification.Of(EScriptAccess.Object);
        }
        if (IsStructHandle(Type) && IsEngineMirror(Type))
        {
            return FScriptPropertyClassification.Of(EScriptAccess.NativeStruct);
        }

        // Stays a managed field because it owns the script's subscriptions and polled state, which native cannot hold.
        if (DerivesFromInputBinding(Type))
        {
            return Type.IsAbstract
                ? FScriptPropertyClassification.Reject(
                    "SInputBinding is abstract. Declare SInputAction for a button or SInputAxis for an axis.")
                : FScriptPropertyClassification.Of(EScriptAccess.InputBinding);
        }
        // Lumina.FString is stored as one native FString, exactly like a C# string member.
        if (Type.ToDisplayString() == ScriptPropertyTypeNames.FString)
        {
            return FScriptPropertyClassification.Of(EScriptAccess.String);
        }

        // Named explicitly so the diagnostic can say which view type to write instead.
        if (Type.TypeKind == TypeKind.Array && Type is IArrayTypeSymbol Array)
        {
            return FScriptPropertyClassification.Reject(
                $"an array is a managed copy, not a view over the native storage. Declare "
                + $"{SuggestListView(Array.ElementType)} instead, which views the storage native already owns.");
        }

        // Bound for a call frame, which owns the storage; a member would also need an accessor and a default.
        if (Type is INamedTypeSymbol Maybe
            && Maybe.OriginalDefinition.SpecialType == SpecialType.System_Nullable_T)
        {
            return FScriptPropertyClassification.Reject(
                $"'{Display(Type)}' crosses as a native TOptional, which is bound for a [Function] "
                + "parameter or return but not yet for a [Property].");
        }

        if (Type.ToDisplayString() == ScriptPropertyTypeNames.FInstancedStruct)
        {
            return FScriptPropertyClassification.Of(EScriptAccess.InstancedStruct);
        }

        if (Type is INamedTypeSymbol Named && Named.IsGenericType)
        {
            switch (Named.ConstructedFrom.ToDisplayString())
            {
                case ScriptPropertyTypeNames.TInstancedStruct:
                    return FScriptPropertyClassification.Of(EScriptAccess.InstancedStruct);

                // A bare pointer that would otherwise pass as blittable and be copied without taking a reference.
                case ScriptPropertyTypeNames.ObjectPtr:
                    return FScriptPropertyClassification.Of(EScriptAccess.ObjectPtr);

                case ScriptPropertyTypeNames.TVector:
                    return ClassifyList(EScriptAccess.ListView, Named.TypeArguments[0]);

                case ScriptPropertyTypeNames.THashMap:
                    return ClassifyMap(Named.TypeArguments[0], Named.TypeArguments[1]);

                case ScriptPropertyTypeNames.List:
                    return FScriptPropertyClassification.Reject(
                        $"List<{Display(Named.TypeArguments[0])}> is a managed copy, not a view over the native "
                        + $"storage. Declare {SuggestListView(Named.TypeArguments[0])} instead.");

                case ScriptPropertyTypeNames.Dictionary:
                    return FScriptPropertyClassification.Reject(
                        $"Dictionary<{Display(Named.TypeArguments[0])}, {Display(Named.TypeArguments[1])}> is a "
                        + "managed copy, not a view over the native storage. Declare the matching "
                        + "THashMap<K, V> instead.");
            }
        }

        // A struct declared in script may hold a string, which makes it managed without making it unsupported.
        if (!Type.IsUnmanagedType && Type.TypeKind != TypeKind.Struct)
        {
            return FScriptPropertyClassification.Reject(SupportedTypesHelp);
        }
        if (Type.IsUnmanagedType && IsEngineMirror(Type))
        {
            return FScriptPropertyClassification.Of(EScriptAccess.Blittable);
        }

        string? Why = ScriptStructRejection(Type, out bool bMarshalled);
        return Why != null
            ? FScriptPropertyClassification.Reject(Why)
            : FScriptPropertyClassification.Of(bMarshalled ? EScriptAccess.MarshalledStruct : EScriptAccess.Blittable);
    }

    // A [Property(Component = ...)] member is a reference to the owner's component rather than a value of that type.
    public static FScriptPropertyClassification ClassifyMember(ISymbol Member, ITypeSymbol Type)
    {
        bool bRefType = IsComponentRef(Type);
        if (!BindsComponent(Member))
        {
            // Without this the reference reads as a script struct, and the advice to mark it [Reflect] leads nowhere.
            return bRefType
                ? FScriptPropertyClassification.Reject($"'{Display(Type)}' is a component reference, so the [Property] needs "
                    + "Component = EComponentBind.Get to find the entity's component, or EComponentBind.GetOrAdd to add it when missing.")
                : Classify(Type);
        }
        bool bComponentRef = bRefType && IsComponent(((INamedTypeSymbol)Type).TypeArguments[0]);
        return bComponentRef
            ? FScriptPropertyClassification.Of(EScriptAccess.ComponentRef)
            : FScriptPropertyClassification.Reject($"'{Display(Type)}' is not a TComponentRef<T> of a component, and Component binds a reference to one of the entity's components.");
    }

    public static bool IsComponent(ITypeSymbol Type)
    {
        return Type.TypeKind == TypeKind.Struct
            && Type.AllInterfaces.Any(IsComponentInterface);
    }

    public static bool IsComponentInterface(ITypeSymbol Interface)
    {
        return Interface.OriginalDefinition.ToDisplayString() == ScriptPropertyTypeNames.Component;
    }

    private static bool BindsComponent(ISymbol Member)
    {
        foreach (AttributeData Attribute in Member.GetAttributes())
        {
            if (Attribute.AttributeClass?.ToDisplayString() != ScriptPropertyTypeNames.PropertyAttribute)
            {
                continue;
            }
            foreach (KeyValuePair<string, TypedConstant> Argument in Attribute.NamedArguments)
            {
                if (Argument.Key == "Component" && Argument.Value.Value is int Bind && Bind != 0)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // A primitive is its bytes everywhere, and a mirror's size is checked against native at startup.
    private static bool IsEngineMirror(ITypeSymbol Type)
    {
        return Type.SpecialType != SpecialType.None || Type.TypeKind == TypeKind.Enum || HasMirrorAttribute(Type);
    }

    private static bool IsReflected(ITypeSymbol Type)
    {
        return FindAttribute(Type, ScriptPropertyTypeNames.ReflectAttribute) != null;
    }

    private static bool HasMirrorAttribute(ITypeSymbol Type)
    {
        foreach (AttributeData Attribute in Type.GetAttributes())
        {
            string? Name = Attribute.AttributeClass?.ToDisplayString();
            if (Name == ScriptPropertyTypeNames.NativeTypeAttribute
                || Name == ScriptPropertyTypeNames.NativeLayoutAttribute)
            {
                return true;
            }
        }
        return false;
    }

    // Native packs members at their own alignment in declaration order, which a sequential C# struct also does.
    private static string? ScriptStructRejection(ITypeSymbol Type, out bool bMarshalled)
    {
        bMarshalled = false;
        if (Type.TypeKind != TypeKind.Struct)
        {
            return $"'{Display(Type)}' cannot back a property. {SupportedTypesHelp}";
        }
        if (!IsReflected(Type))
        {
            return $"'{Display(Type)}' is a struct declared in script, and native stores it by its reflected type. Mark it [Reflect].";
        }

        bool bAnyField = false;
        foreach (ISymbol Member in Type.GetMembers())
        {
            if (Member is not IFieldSymbol Field || Field.IsStatic || Field.IsConst || Field.IsImplicitlyDeclared)
            {
                continue;
            }
            bAnyField = true;

            if (!IsStoredStructField(Field))
            {
                return $"'{Display(Type)}' is a struct declared in script, so every one of its fields is part "
                     + $"of the value native stores, but '{Field.Name}' has no [Property]. "
                     + "Native would leave it out of the layout while C# still reads it, so the "
                     + "accessor would run off the end of the storage. Mark it, or move it out of the struct.";
            }

            FScriptPropertyClassification FieldClass = Classify(Field.Type);
            if (!FieldClass.IsSupported)
            {
                return $"'{Display(Type)}' has the field '{Field.Name}', and {FieldClass.Rejection}";
            }
            EScriptAccess MemberAccess = FieldClass.Access;
            if (MemberAccess is EScriptAccess.String or EScriptAccess.MarshalledStruct)
            {
                bMarshalled = true;
                continue;
            }
            if (MemberAccess != EScriptAccess.Blittable && MemberAccess != EScriptAccess.Enum)
            {
                return $"'{Display(Type)}' has the field '{Field.Name}' of type "
                     + $"'{Display(Field.Type)}', which a script struct cannot hold. A struct declared in script "
                     + "may hold only numbers, bool, enums, strings, engine math types, and structs of those.";
            }
        }

        if (!bAnyField)
        {
            return $"'{Display(Type)}' has no fields, so there is nothing for native to store.";
        }
        return null;
    }

    private static bool IsStoredStructField(IFieldSymbol Field)
    {
        return RuleFor(Field, EScriptMemberKind.Field, bOwnerIsNativeObject: false).bStored;
    }

    // ScriptMemberRules over a Roslyn symbol, as TypeLibrary.RuleFor does by reflection.
    public static FScriptMemberRule RuleFor(ISymbol Member, EScriptMemberKind Kind, bool bOwnerIsNativeObject)
    {
        HashSet<string>? Specifiers = SpecifiersOf(Member, ScriptPropertyTypeNames.PropertyAttribute);
        return ScriptMemberRules.Classify(Specifiers != null ? Specifiers.Contains : null, Kind, bOwnerIsNativeObject);
    }

    // The bare specifiers and named arguments of one LuminaSharp attribute, or null when the member does not carry it.
    public static HashSet<string>? SpecifiersOf(ISymbol Member, string AttributeName)
    {
        AttributeData? Attribute = FindAttribute(Member, AttributeName);
        if (Attribute == null)
        {
            return null;
        }
        var Names = new HashSet<string>(BareSpecifiers(Attribute));
        foreach (KeyValuePair<string, TypedConstant> Named in Attribute.NamedArguments)
        {
            Names.Add(Named.Key);
        }
        return Names;
    }

    public static IEnumerable<string> BareSpecifiers(AttributeData Attribute)
    {
        foreach (TypedConstant Argument in Attribute.ConstructorArguments)
        {
            IEnumerable<TypedConstant> Values = Argument.Kind == TypedConstantKind.Array ? Argument.Values : new[] { Argument };
            foreach (TypedConstant Value in Values)
            {
                if (EnumMemberName(Value) is { } Name)
                {
                    yield return Name;
                }
            }
        }
    }

    public static AttributeData? FindAttribute(ISymbol Member, string AttributeName)
    {
        return Member.GetAttributes().FirstOrDefault(Data => Data.AttributeClass?.ToDisplayString() == AttributeName);
    }

    public static object? NamedArgument(AttributeData Attribute, string Key)
    {
        foreach (KeyValuePair<string, TypedConstant> Named in Attribute.NamedArguments)
        {
            if (Named.Key == Key)
            {
                return Named.Value.Value;
            }
        }
        return null;
    }

    private static string? EnumMemberName(TypedConstant Value)
    {
        if (Value.Type is not INamedTypeSymbol { TypeKind: TypeKind.Enum } Enum || Value.Value == null)
        {
            return null;
        }
        return Enum.GetMembers().OfType<IFieldSymbol>().FirstOrDefault(Field => Field.HasConstantValue && Equals(Field.ConstantValue, Value.Value))?.Name;
    }

    private static FScriptPropertyClassification ClassifyList(EScriptAccess Access, ITypeSymbol Element)
    {
        string? Why = ElementRejection(Element, "element");
        return Why != null
            ? FScriptPropertyClassification.Reject(Why)
            : FScriptPropertyClassification.List(Access, Element);
    }

    // Both sides route through ElementMarshal now, so a map carries the same element kinds a vector does.
    private static FScriptPropertyClassification ClassifyMap(ITypeSymbol Key, ITypeSymbol Value)
    {
        string? Why = ElementRejection(Key, "key")
                   ?? ElementRejection(Value, "value");
        return Why != null
            ? FScriptPropertyClassification.Reject(Why)
            : FScriptPropertyClassification.Map(Key, Value);
    }

    // Elements are copied by raw bytes with no setter bookkeeping, so passing IsUnmanagedType is not enough.
    private static string? ElementRejection(ITypeSymbol Type, string Position)
    {
        FScriptPropertyClassification Classification = Classify(Type);
        if (Classification.Access == EScriptAccess.Blittable)
        {
            return null;
        }
        if (!Classification.IsSupported)
        {
            return $"the {Position} type is refused, because {Classification.Rejection}";
        }

        // An enum slot is its underlying type's width now, so the stride matches and it packs like a number.
        if (Classification.Access == EScriptAccess.Enum)
        {
            return null;
        }

        // A container goes through each element's own accessors, so it can carry these non-byte kinds.
        if (Type.ToDisplayString() == ScriptPropertyTypeNames.FString)
        {
            return null;
        }
        if (Classification.Access is EScriptAccess.ObjectPtr or EScriptAccess.MarshalledStruct)
        {
            return null;
        }

        string Prefix = $"the {Position} type '{Display(Type)}' cannot live inside a container: ";
        return Classification.Access switch
        {
            // A managed string cannot live in native memory; FString names the native slot instead.
            EScriptAccess.String =>
                Prefix + "a managed string cannot live in native memory. Declare the element as "
                       + "Lumina.FString, which is the native string itself.",

            // A bare wrapper is not a storable reference, since a native object slot holds a TStrongObjectPtr.
            EScriptAccess.Object =>
                Prefix + "a bare wrapper is not a storable reference. Declare the element as "
                       + $"TStrongObjectPtr<{Referenced(Type)}>, which is what the native slot holds.",

            EScriptAccess.AssetPath =>
                Prefix + "an asset reference is stored as a path, not as bytes.",

            EScriptAccess.InputBinding =>
                Prefix + "an input binding is a managed object; native stores only its action name.",

            EScriptAccess.ListView or EScriptAccess.MapView =>
                Prefix + "it is itself a container, and the native property system has no nested-container "
                       + "property. Give the elements a struct type that holds what you need instead.",

            _ => Prefix + "it is not a plain value.",
        };
    }

    // Still names TVector<T> for an element no container accepts, since declaring it earns the specific rejection.
    private static string SuggestListView(ITypeSymbol Element)
    {
        EScriptAccess Access = Classify(Element).Access;
        if (Access == EScriptAccess.String)
        {
            return "TVector<Lumina.FString>";
        }
        if (Access == EScriptAccess.Object || Access == EScriptAccess.ObjectPtr)
        {
            return $"TVector<TStrongObjectPtr<{Referenced(Element)}>>";
        }
        return $"TVector<{Display(Element)}>";
    }

    // Unwraps a TStrongObjectPtr so suggested declarations never double-wrap it.
    private static string Referenced(ITypeSymbol Type)
    {
        if (Type is INamedTypeSymbol Named && Named.IsGenericType
            && Named.ConstructedFrom.ToDisplayString() == ScriptPropertyTypeNames.ObjectPtr)
        {
            return Display(Named.TypeArguments[0]);
        }
        return Display(Type);
    }

    public static bool IsSoftObjectReference(ITypeSymbol Type)
    {
        return Type.AllInterfaces.Any(Interface => Interface.ToDisplayString() == ScriptPropertyTypeNames.SoftObjectReference);
    }

    public static bool DerivesFromInputBinding(ITypeSymbol Type)
    {
        for (ITypeSymbol? Current = Type; Current != null; Current = Current.BaseType)
        {
            if (Current.ToDisplayString() == ScriptPropertyTypeNames.InputBinding)
            {
                return true;
            }
        }
        return false;
    }

    public static bool DerivesFromNativeObject(ITypeSymbol Type)
    {
        for (INamedTypeSymbol? Base = Type.BaseType; Base != null; Base = Base.BaseType)
        {
            if (Base.ToDisplayString() == ScriptPropertyTypeNames.NativeObject)
            {
                return true;
            }
        }
        return false;
    }

    public static bool IsNativeObject(ITypeSymbol Type)
    {
        return Type.WithNullableAnnotation(NullableAnnotation.NotAnnotated).ToDisplayString() == ScriptPropertyTypeNames.NativeObject
            || DerivesFromNativeObject(Type);
    }

    public static bool IsComponentRef(ITypeSymbol Type)
    {
        return Type is INamedTypeSymbol { IsGenericType: true } Named
            && Named.OriginalDefinition.ToDisplayString() == ScriptPropertyTypeNames.ComponentRef;
    }

    // Another method of the same name in the same type that also carries the attribute, so both would generate one member name.
    public static bool HasAttributedTwin(IMethodSymbol Method, string AttributeName)
    {
        return Method.ContainingType.GetMembers(Method.Name).OfType<IMethodSymbol>().Count(Candidate =>
            FindAttribute(Candidate, AttributeName) != null) > 1;
    }

    // A struct handle views native memory, so a member of its type is the embedded struct rather than a copy of its bytes.
    public static bool IsStructHandle(ITypeSymbol Type)
    {
        return Type.TypeKind == TypeKind.Struct
            && Type.AllInterfaces.Any(Interface => Interface.ToDisplayString() == ScriptPropertyTypeNames.NativeStruct);
    }

    private static string Display(ITypeSymbol Type) => Type.ToDisplayString();

    public const string SupportedTypesHelp =
        "it cannot be viewed over native storage. Supported: numbers, bool, enums, engine struct mirrors, "
        + "a struct you declare whose every field is marked and blittable, string, Lumina.FString, "
        + "Lumina.FName, asset references, object references, and TVector<T> or THashMap<K, V> "
        + "whose elements are plain values, enums, Lumina.FString, TStrongObjectPtr<T>, or a struct you declare.";
}

// Mirrors the native property kinds.
internal enum EScriptAccess
{
    Unsupported,
    Blittable,
    Enum,
    String,
    AssetPath,
    Object,
    NativeStruct,
    ObjectPtr,
    ListView,
    MapView,
    InputBinding,

    // A struct declared in script holding a string, so its native layout differs from its managed one and each field crosses on its own.
    MarshalledStruct,

    // A reference to the owner entity's component, bound before OnAttach and read as the handle that tracks it.
    ComponentRef,

    // A view over a native FInstancedStruct slot, whose held type is picked at runtime.
    InstancedStruct,
}

internal readonly struct FScriptPropertyClassification
{
    public EScriptAccess Access { get; }

    public ITypeSymbol? Element { get; }

    public ITypeSymbol? Key { get; }

    public ITypeSymbol? Value { get; }

    // A sentence that completes the diagnostic after the property and its type are named.
    public string? Rejection { get; }

    private FScriptPropertyClassification(EScriptAccess Access, ITypeSymbol? Element, ITypeSymbol? Key,
        ITypeSymbol? Value, string? Rejection)
    {
        this.Access = Access;
        this.Element = Element;
        this.Key = Key;
        this.Value = Value;
        this.Rejection = Rejection;
    }

    public static FScriptPropertyClassification Of(EScriptAccess Access) => new(Access, null, null, null, null);

    public static FScriptPropertyClassification List(EScriptAccess Access, ITypeSymbol Element) =>
        new(Access, Element, null, null, null);

    public static FScriptPropertyClassification Map(ITypeSymbol Key, ITypeSymbol Value) =>
        new(EScriptAccess.MapView, null, Key, Value, null);

    public static FScriptPropertyClassification Reject(string Why) =>
        new(EScriptAccess.Unsupported, null, null, null, Why);

    public bool IsSupported => Access != EScriptAccess.Unsupported;

    // A view over storage native owns, so it has no setter and no default.
    public bool IsView => ScriptPropertyTypeNames.IsViewAccess(Access);

    // Left a plain field by the rewriter; native mints the property and the serializer syncs it.
    public bool KeepsManagedField => Access == EScriptAccess.InputBinding;
}

// Roslyn display strings, since this netstandard2.0 file cannot reference LuminaSharp to use typeof.
internal static class ScriptPropertyTypeNames
{
    public const string NativeObject = "LuminaSharp.NativeObject";
    public const string NativeStruct = "LuminaSharp.INativeStruct";
    public const string NativeStructRoot = "LuminaSharp.INativeStruct<TSelf>";
    public const string Extends = "LuminaSharp.IExtends<TBase>";
    public const string Component = "LuminaSharp.IComponent<TSelf>";
    public const string ComponentRef = "LuminaSharp.TComponentRef<T>";
    public const string SoftObjectReference = "LuminaSharp.ISoftObjectReference";
    public const string InputBinding = "LuminaSharp.SInputBinding";

    // What marks a struct as an engine mirror of a native type, so reading it as raw bytes is sound.
    public const string NativeTypeAttribute = "LuminaSharp.NativeTypeAttribute";
    public const string NativeLayoutAttribute = "LuminaSharp.NativeLayoutAttribute";
    public const string ReflectAttribute = "LuminaSharp.ReflectAttribute";
    public const string NativeSlotViewAttribute = "LuminaSharp.NativeSlotViewAttribute";

    public const string PropertyAttribute = "LuminaSharp.PropertyAttribute";
    public const string FunctionAttribute = "LuminaSharp.FunctionAttribute";

    public const string FString = "Lumina.FString";
    public const string TVector = "Lumina.TVector<T>";
    public const string THashMap = "Lumina.THashMap<K, V>";
    public const string ObjectPtr = "Lumina.TStrongObjectPtr<T>";
    public const string FInstancedStruct = "Lumina.FInstancedStruct";
    public const string TInstancedStruct = "Lumina.TInstancedStruct<TBase>";

    public const string List = "System.Collections.Generic.List<T>";
    public const string Dictionary = "System.Collections.Generic.Dictionary<TKey, TValue>";

    // ScriptPropertyViews checks its typeof table against these, so a renamed view type fails loudly.
    public const string TVectorMetadata = "Lumina.TVector`1";
    public const string THashMapMetadata = "Lumina.THashMap`2";

    // The runtime-side table asserts it covers exactly these view kinds.
    public static IReadOnlyList<EScriptAccess> ViewAccessKinds { get; } = new[]
    {
        EScriptAccess.ListView,
        EScriptAccess.MapView,
    };

    public static bool IsViewAccess(EScriptAccess Access) =>
        Access is EScriptAccess.ListView or EScriptAccess.MapView or EScriptAccess.InstancedStruct;
}
