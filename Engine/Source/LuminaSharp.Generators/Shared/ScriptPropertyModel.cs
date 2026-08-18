using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;

namespace LuminaSharp.ScriptProperties;

/// <summary>
/// The one classifier that decides what a <c>[Property]</c> member's type means, shared by source between the
/// IDE analyzer (LuminaSharp.Generators) and the rewriter that emits the accessors (LuminaSharp).
///
/// Why shared by source and not by reference. The generator is a Roslyn component: LuminaSharp references it
/// with <c>ReferenceOutputAssembly="false"</c>, so it cannot see the generator's types, and the generator
/// targets netstandard2.0 so it cannot see LuminaSharp's. Both, however, already reference
/// Microsoft.CodeAnalysis and both classify <see cref="ITypeSymbol"/>s -- so one file, linked into both, is
/// exactly the amount of sharing the constraint allows. Keep it dependency-free beyond Roslyn.
///
/// Why it has to be shared at all. The analyzer's whole job is to say, at the declaration, what the rewriter
/// will say at script load. Two implementations of that answer drift, and they had: an element type was tested
/// with <c>IsUnmanagedType</c> on the analyzer side and with "classifies as Blittable" on the rewriter side,
/// which disagreed about nested containers (the IDE accepted <c>TVector&lt;TVector&lt;float&gt;&gt;</c>,
/// the engine's compile refused it) and about enums (the IDE accepted <c>TVector&lt;EMode&gt;</c>, whose
/// elements native mints 8 bytes wide while C# would read them 4 wide). There is now one answer.
/// </summary>
internal static class ScriptPropertyClassifier
{
    /// <summary>Classifies a member type. Never throws; an unsupported type comes back with a
    /// <see cref="FScriptPropertyClassification.Rejection"/> explaining what to write instead.</summary>
    public static FScriptPropertyClassification Classify(ITypeSymbol Type)
    {
        if (Type.TypeKind == TypeKind.Enum)
        {
            return FScriptPropertyClassification.Of(EScriptAccess.Enum);
        }
        if (Type.SpecialType == SpecialType.System_String)
        {
            return FScriptPropertyClassification.Of(EScriptAccess.String);
        }

        // Asset references (FSoftObjectPath, TSoftObjectPtr<T>; TObjectPtr<T> is NOT one) are recognised by the
        // interface they share, so a new asset-reference type needs nothing here. Checked before the unmanaged
        // test at the bottom, which they would otherwise pass -- they are pointer-sized values, and reading
        // them as raw bytes would not read the FSoftObjectPath native actually stores.
        if (IsAssetRef(Type))
        {
            return FScriptPropertyClassification.Of(EScriptAccess.AssetPath);
        }
        if (DerivesFromNativeObject(Type))
        {
            return FScriptPropertyClassification.Of(EScriptAccess.Object);
        }

        // Stays a managed field: it owns the script's subscriptions and polled state, which native cannot hold.
        if (DerivesFromInputBinding(Type))
        {
            return Type.IsAbstract
                ? FScriptPropertyClassification.Reject(
                    "SInputBinding is abstract. Declare SInputAction for a button or SInputAxis for an axis.")
                : FScriptPropertyClassification.Of(EScriptAccess.InputBinding);
        }
        // The explicit mirror of Lumina::FString. Stored exactly as a C# `string` member is -- one native
        // FString -- so it classifies the same; the difference is only which spelling the author wrote.
        if (Type.ToDisplayString() == ScriptPropertyTypeNames.FString)
        {
            return FScriptPropertyClassification.Of(EScriptAccess.String);
        }

        // Named explicitly so the diagnostic can say which view type to write instead of falling through to
        // the generic list. An array and a List<T> are managed copies; nothing else would explain that.
        if (Type.TypeKind == TypeKind.Array && Type is IArrayTypeSymbol Array)
        {
            return FScriptPropertyClassification.Reject(
                $"an array is a managed copy, not a view over the native storage. Declare "
                + $"{SuggestListView(Array.ElementType)} instead, which views the storage native already owns.");
        }

        if (Type is INamedTypeSymbol Named && Named.IsGenericType)
        {
            switch (Named.ConstructedFrom.ToDisplayString())
            {
                // A hard object reference. Stored natively as an object property, so it keeps its target
                // alive. Checked before the unmanaged test below: it is a bare pointer, so it would otherwise
                // pass as blittable and be copied WITHOUT taking a reference.
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

        // Everything blittable -- numbers, bool, and the struct mirrors like FVector3/FTransform -- is read in
        // place at the property's offset. Checked last so every special case above wins.
        return Type.IsUnmanagedType
            ? FScriptPropertyClassification.Of(EScriptAccess.Blittable)
            : FScriptPropertyClassification.Reject(SupportedTypesHelp);
    }

    private static FScriptPropertyClassification ClassifyList(EScriptAccess Access, ITypeSymbol Element)
    {
        string? Why = ElementRejection(Element, "element", bMarshalled: true);
        return Why != null
            ? FScriptPropertyClassification.Reject(Why)
            : FScriptPropertyClassification.List(Access, Element);
    }

    // A map's key and value are still plain values only. TVector gained marshalled elements because its view
    // routes every slot through ElementMarshal; THashMap's does not yet, and claiming support the view cannot
    // honour would corrupt rather than fail.
    private static FScriptPropertyClassification ClassifyMap(ITypeSymbol Key, ITypeSymbol Value)
    {
        string? Why = ElementRejection(Key, "key", bMarshalled: false)
                   ?? ElementRejection(Value, "value", bMarshalled: false);
        return Why != null
            ? FScriptPropertyClassification.Reject(Why)
            : FScriptPropertyClassification.Map(Key, Value);
    }

    /// <summary>
    /// Why <paramref name="Type"/> may not sit inside a container, or null if it may.
    ///
    /// The bar is higher than for a member, and deliberately so: a container element is stored in the native
    /// buffer at the stride the ops table reports and copied by raw bytes, with none of the bookkeeping a
    /// property setter would do. So an element must classify as <see cref="EScriptAccess.Blittable"/> -- NOT
    /// merely pass <c>IsUnmanagedType</c>, which several supported MEMBER types also pass while meaning
    /// something else in memory. Each of those gets its own message, because in every case there is a
    /// specific right answer and the generic list would not name it.
    /// </summary>
    private static string? ElementRejection(ITypeSymbol Type, string Position, bool bMarshalled)
    {
        FScriptPropertyClassification Classification = Classify(Type);
        if (Classification.Access == EScriptAccess.Blittable)
        {
            return null;
        }

        // A marshalled container reads and writes each slot through the element's own accessors, so it can
        // carry the two kinds whose managed value is not their native bytes.
        if (bMarshalled && Type.ToDisplayString() == ScriptPropertyTypeNames.FString)
        {
            return null;
        }
        if (bMarshalled && Classification.Access == EScriptAccess.ObjectPtr)
        {
            return null;
        }

        string Prefix = $"the {Position} type '{Display(Type)}' cannot live inside a container: ";
        return Classification.Access switch
        {
            // A managed string cannot live in native memory; FString names the native slot instead.
            EScriptAccess.String when bMarshalled =>
                Prefix + "a managed string cannot live in native memory. Declare the element as "
                       + "Lumina.FString, which is the native string itself.",

            EScriptAccess.String =>
                Prefix + "it is a managed reference, which cannot live in native memory.",

            // Native mints an enum property as a 64-bit slot whatever the C# underlying type is (see the
            // rewriter's Enum accessor). A member survives that because its accessor reads the whole slot,
            // but an element cannot: the span would walk 4-byte strides over 8-byte elements. Give the
            // element an explicitly-sized integer and cast, or a struct wrapping one.
            EScriptAccess.Enum =>
                Prefix + "native stores an enum property in a 64-bit slot, so its stride would not match the "
                       + "C# underlying type. Use a TVector<long> and cast, or a struct element.",

            // A bare wrapper is not a storable reference -- TObjectPtr is what a native object slot holds.
            EScriptAccess.Object when bMarshalled =>
                Prefix + "a bare wrapper is not a storable reference. Declare the element as "
                       + $"TObjectPtr<{Referenced(Type)}>, which is what the native slot holds.",

            EScriptAccess.Object or EScriptAccess.ObjectPtr =>
                Prefix + "it is an object reference, and copying it by bytes would store the pointer without "
                       + "taking a reference.",

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

    /// <summary>The list view that would accept <paramref name="Element"/>, for the array/List&lt;T&gt;
    /// diagnostics. Best-effort advice: if the element is unusable in any container, TVector&lt;T&gt; is
    /// still the right thing to name, and declaring it earns the specific message from
    /// <see cref="ElementRejection"/>.</summary>
    private static string SuggestListView(ITypeSymbol Element)
    {
        EScriptAccess Access = Classify(Element).Access;
        if (Access == EScriptAccess.String)
        {
            return "TVector<Lumina.FString>";
        }
        if (Access == EScriptAccess.Object || Access == EScriptAccess.ObjectPtr)
        {
            return $"TVector<TObjectPtr<{Referenced(Element)}>>";
        }
        return $"TVector<{Display(Element)}>";
    }

    /// <summary>The wrapper type an object reference points AT: the type argument of a TObjectPtr&lt;T&gt;,
    /// or the type itself for a bare wrapper. Advice that names TObjectPtr&lt;T&gt; must not double-wrap an
    /// element that already is one, or the suggested declaration would not compile either.</summary>
    private static string Referenced(ITypeSymbol Type)
    {
        if (Type is INamedTypeSymbol Named && Named.IsGenericType
            && Named.ConstructedFrom.ToDisplayString() == ScriptPropertyTypeNames.ObjectPtr)
        {
            return Display(Named.TypeArguments[0]);
        }
        return Display(Type);
    }

    public static bool IsAssetRef(ITypeSymbol Type)
    {
        return Type.AllInterfaces.Any(Interface => Interface.ToDisplayString() == ScriptPropertyTypeNames.AssetRef);
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

    private static string Display(ITypeSymbol Type) => Type.ToDisplayString();

    /// <summary>The fallback explanation, for a type that matches nothing at all.</summary>
    public const string SupportedTypesHelp =
        "it cannot be viewed over native storage. Supported: numbers, bool, enums, blittable struct mirrors, "
        + "string, Lumina.FString, Lumina.FName, asset references, object references, TVector<T> (of a plain value, an "
        + "FString, or a TObjectPtr<T>), and THashMap<K, V> of plain values.";
}

/// <summary>How one member's value is reached. Mirrors the native property kinds; see the rewriter's emitter.</summary>
internal enum EScriptAccess
{
    Unsupported,
    Blittable,
    Enum,
    String,
    AssetPath,
    Object,
    ObjectPtr,
    ListView,
    MapView,
    InputBinding,
}

/// <summary>The result of <see cref="ScriptPropertyClassifier.Classify"/>: the access shape plus whatever type
/// arguments the emitter needs, or a rejection explaining what to write instead.</summary>
internal readonly struct FScriptPropertyClassification
{
    public EScriptAccess Access { get; }

    /// <summary>Element type for <see cref="EScriptAccess.ListView"/>. Null otherwise.</summary>
    public ITypeSymbol? Element { get; }

    /// <summary>Key type for <see cref="EScriptAccess.MapView"/>, null otherwise.</summary>
    public ITypeSymbol? Key { get; }

    /// <summary>Value type for <see cref="EScriptAccess.MapView"/>, null otherwise.</summary>
    public ITypeSymbol? Value { get; }

    /// <summary>Why the type was refused, as a sentence completing "[Property] 'X' has type 'Y': ...".
    /// Non-null exactly when <see cref="Access"/> is <see cref="EScriptAccess.Unsupported"/>.</summary>
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

    /// <summary>A container property: a view over storage native owns, so it has no setter and no default.</summary>
    public bool IsView => ScriptPropertyTypeNames.IsViewAccess(Access);

    // Left a plain field by the rewriter; native mints the property and the serializer syncs it.
    public bool KeepsManagedField => Access == EScriptAccess.InputBinding;
}

/// <summary>
/// Every type this layer matches by name, declared once.
///
/// They are Roslyn display strings because that is what the two classifying sites have to compare against;
/// the runtime side (TypeLibrary) matches the same types with <c>typeof</c>, which cannot be spelled here --
/// netstandard2.0, and no reference to LuminaSharp. <c>ScriptPropertyViews</c> on that side cross-checks its
/// <c>typeof</c> table against the metadata names below, so renaming or moving one of these types fails loudly
/// instead of silently un-matching.
/// </summary>
internal static class ScriptPropertyTypeNames
{
    public const string NativeObject = "LuminaSharp.NativeObject";
    public const string AssetRef = "LuminaSharp.IAssetRef";
    public const string InputBinding = "LuminaSharp.SInputBinding";

    public const string FString = "Lumina.FString";
    public const string TVector = "Lumina.TVector<T>";
    public const string THashMap = "Lumina.THashMap<K, V>";
    public const string ObjectPtr = "Lumina.TObjectPtr<T>";

    public const string List = "System.Collections.Generic.List<T>";
    public const string Dictionary = "System.Collections.Generic.Dictionary<TKey, TValue>";

    /// <summary>Metadata (reflection) names for the view types, so the runtime-side table can prove it is
    /// matching the same types this file matches by display name.</summary>
    public const string TVectorMetadata = "Lumina.TVector`1";
    public const string THashMapMetadata = "Lumina.THashMap`2";

    /// <summary>The access kinds that are container views. The runtime-side table asserts it covers exactly
    /// these, so a view kind added to the classifier cannot be missed there.</summary>
    public static IReadOnlyList<EScriptAccess> ViewAccessKinds { get; } = new[]
    {
        EScriptAccess.ListView,
        EScriptAccess.MapView,
    };

    public static bool IsViewAccess(EScriptAccess Access) =>
        Access is EScriptAccess.ListView or EScriptAccess.MapView;
}
