using System;
using System.Collections.Generic;
using System.Linq;
using Lumina;
using LuminaSharp.ScriptProperties;

namespace LuminaSharp;

/// <summary>
/// The container view types a <c>[Property]</c> may be declared as, as one table.
///
/// This is the runtime-reflection half of what <c>ScriptPropertyClassifier</c> decides over Roslyn symbols.
/// It cannot be the same code -- the classifier is shared with a netstandard2.0 analyzer that cannot reference
/// these types, and matching them there means matching display names. So the two halves meet at
/// <see cref="EScriptAccess"/> and are held together by <see cref="Validate"/>, which runs before the first
/// type is described and fails loudly if they have come apart.
///
/// One table rather than a chain of ifs per question, because the three questions asked about these types --
/// "is it a view?", "what is its element?", "what are its key and value?" -- were each answered by their own
/// list, and a view taught to one list and not another is not a compile error, just a property that silently
/// describes wrong.
/// </summary>
internal static class ScriptPropertyViews
{
    /// <summary>One view type: the open generic definition, how the classifier reports it, and the arity
    /// native sees on the wire.</summary>
    private readonly struct FView
    {
        public readonly Type Definition;
        public readonly EScriptAccess Access;

        /// <summary>The metadata name the shared classifier matches this type by. Cross-checked against
        /// <see cref="Type.FullName"/> so moving or renaming the type cannot silently stop matching.</summary>
        public readonly string MetadataName;

        /// <summary>Fixed element type for a view whose element is not a type argument, else null -- take it
        /// from the type arguments. Nothing needs it today; kept because a future view might.</summary>
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
        // One list view for every element flavour: a plain value, an FString, or a TObjectPtr<T>. The element
        // type is the type argument in all three cases -- TVector routes the per-slot read/write through
        // ElementMarshal, so there is nothing to special-case here either.
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

    /// <summary>True for a type that IS a view over native storage rather than a managed value, so a
    /// get-only member of it is still a real property.</summary>
    public static bool IsView(Type Type) => Find(Type) != null;

    /// <summary>The element type of a list view, or false if <paramref name="Type"/> is not one (a map is
    /// not; ask <see cref="TryGetKeyValue"/>).</summary>
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

    /// <summary>The key and value types of a map view, or false if <paramref name="Type"/> is not one.</summary>
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

    /// <summary>
    /// Proves this table still describes the same types the shared classifier matches. Two ways it can come
    /// apart, and neither is a compile error on its own:
    ///
    /// 1. A view kind is added to the classifier and not here -- the rewriter emits an accessor for it, and
    ///    the schema this side reports never mentions it, so native mints nothing to back it.
    /// 2. One of these types is renamed or moved -- <c>typeof</c> follows it, the classifier's display-string
    ///    match does not, and every property of that type quietly becomes "unsupported".
    ///
    /// Throws rather than logging: both are engine bugs a developer just introduced, and both produce
    /// properties that silently do not work if allowed to run.
    /// </summary>
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
