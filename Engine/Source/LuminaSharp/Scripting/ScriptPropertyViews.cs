using System;
using System.Collections.Generic;
using System.Linq;
using Lumina;
using LuminaSharp.ScriptProperties;

namespace LuminaSharp;

// The container view types a [Property] may be, one table answering every question about them, which Validate holds to the shared classifier.
internal static class ScriptPropertyViews
{
    // One view type, its open generic definition, how the classifier reports it, and its arity.
    private readonly struct FView
    {
        public readonly Type Definition;
        public readonly EScriptAccess Access;

        // The name the shared classifier matches this type by, checked against Type.FullName so a rename cannot silently stop matching.
        public readonly string MetadataName;

        // The element type of a view whose element is not a type argument, else null.
        public readonly Type? FixedElement;

        public FView(Type Definition, EScriptAccess Access, string MetadataName, Type? FixedElement = null)
        {
            this.Definition = Definition;
            this.Access = Access;
            this.MetadataName = MetadataName;
            this.FixedElement = FixedElement;
        }
    }

    private static readonly FView[] Views =
    {
        // One list view for every element kind, since TVector routes each slot through ElementMarshal.
        new(typeof(TVector<>), EScriptAccess.ListView,
            ScriptPropertyTypeNames.TVectorMetadata),

        new(typeof(THashMap<,>), EScriptAccess.MapView,
            ScriptPropertyTypeNames.THashMapMetadata),
    };


    private static bool bValidated;

    private static FView? Find(Type Type)
    {
        Type Key = Type.IsGenericType ? Type.GetGenericTypeDefinition() : Type;
        foreach (FView View in Views)
        {
            if (View.Definition == Key)
            {
                return View;
            }
        }
        return null;
    }

    // True for a type viewing native storage, so a get-only member of it is still a real property.
    public static bool IsView(Type Type) => Find(Type) != null;

    public static bool TryGetAccess(Type Type, out EScriptAccess Access)
    {
        if (Find(Type) is not { } View)
        {
            Access = default;
            return false;
        }

        Access = View.Access;
        return true;
    }

    // The element type of a list view, false for anything else including a map.
    public static bool TryGetElementType(Type Type, out Type? ElementType)
    {
        ElementType = null;
        if (Find(Type) is not { } View || View.Access == EScriptAccess.MapView)
        {
            return false;
        }
        ElementType = View.FixedElement ?? Type.GetGenericArguments()[0];
        return true;
    }

    // The key and value types of a map view, or false when Type is not one.
    public static bool TryGetKeyValue(Type Type, out Type? KeyType, out Type? ValueType)
    {
        KeyType = null;
        ValueType = null;
        if (Find(Type) is not { Access: EScriptAccess.MapView })
        {
            return false;
        }
        Type[] Arguments = Type.GetGenericArguments();
        KeyType = Arguments[0];
        ValueType = Arguments[1];
        return true;
    }

    // Throws if this table and the shared classifier have come apart, since either drift leaves properties silently broken.
    public static void Validate()
    {
        if (bValidated)
        {
            return;
        }

        foreach (FView View in Views)
        {
            if (View.Definition.FullName != View.MetadataName)
            {
                throw new InvalidOperationException(
                    $"Script property view '{View.Definition.FullName}' no longer matches the name the shared "
                    + $"classifier looks for ('{View.MetadataName}'). It was renamed or moved; update "
                    + "ScriptPropertyTypeNames in ScriptPropertyModel.cs to match.");
            }
        }

        IReadOnlyList<EScriptAccess> Expected = ScriptPropertyTypeNames.ViewAccessKinds;
        foreach (EScriptAccess Access in Expected)
        {
            if (Views.Count(View => View.Access == Access) != 1)
            {
                throw new InvalidOperationException(
                    $"Script property view kind {Access} is declared by the shared classifier but has no "
                    + "entry (or more than one) in ScriptPropertyViews.Views, so the schema reported to "
                    + "native cannot describe it. Add it to that table.");
            }
        }
        foreach (FView View in Views)
        {
            if (!Expected.Contains(View.Access))
            {
                throw new InvalidOperationException(
                    $"ScriptPropertyViews.Views maps '{View.Definition.Name}' to {View.Access}, which the "
                    + "shared classifier does not consider a container view. Add it to "
                    + "ScriptPropertyTypeNames.ViewAccessKinds or correct the entry.");
            }
        }

        bValidated = true;
    }
}
