using System;
using System.Collections.Generic;
using Lumina;
using LuminaSharp.ScriptProperties;

namespace LuminaSharp;

// The one classifier, so the generic element cache and the call-frame binder cannot disagree about a type.
internal static class ElementKinds
{
    public static EElementKind Of(Type Slot)
    {
        if (Slot == typeof(FString))
        {
            return EElementKind.String;
        }
        if (Slot == typeof(string))
        {
            return EElementKind.ManagedString;
        }
        if (Slot == typeof(bool))
        {
            return EElementKind.Bool;
        }
        // Ahead of the blittable fallback, which would copy the nullable's own flag byte into the slot.
        if (Nullable.GetUnderlyingType(Slot) != null)
        {
            return EElementKind.Optional;
        }
        // Assigning a TObjectPtr by bytes would store the pointer without releasing or taking a reference.
        if (Slot.IsGenericType && Slot.GetGenericTypeDefinition() == typeof(TObjectPtr<>))
        {
            return EElementKind.ObjectRef;
        }
        if (Slot.IsGenericType && Slot.GetGenericTypeDefinition() == typeof(TOptional<>))
        {
            return EElementKind.OptionalView;
        }
        // The view table is the one place a container view is declared, and Validate holds it to the classifier.
        if (ScriptPropertyViews.TryGetAccess(Slot, out EScriptAccess Access))
        {
            return Access == EScriptAccess.MapView ? EElementKind.Map : EElementKind.Vector;
        }
        // A copy, not a view, so it is the one container shape a frame can hand back by value.
        if (Slot.IsArray || (Slot.IsGenericType && Slot.GetGenericTypeDefinition() == typeof(List<>)))
        {
            return EElementKind.VectorCopy;
        }
        if (Slot.IsDefined(typeof(NativeSlotViewAttribute), false))
        {
            return EElementKind.SlotView;
        }
        // Matched by the interface rather than by name, so a new asset-reference type needs no change here.
        if (typeof(IAssetRef).IsAssignableFrom(Slot))
        {
            return EElementKind.SoftRef;
        }
        // A generated wrapper views native memory rather than holding it, so both ends go through a real copy.
        if (typeof(NativeStruct).IsAssignableFrom(Slot))
        {
            return EElementKind.StructView;
        }
        if (typeof(NativeObject).IsAssignableFrom(Slot))
        {
            return EElementKind.ObjectWrapper;
        }
        return EElementKind.Blittable;
    }
}
