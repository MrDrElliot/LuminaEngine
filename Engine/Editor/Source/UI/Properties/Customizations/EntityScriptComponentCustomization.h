#pragma once

#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class CEntityScript;
    class FPropertyTable;

    /**
     * Inspector for SEntityScriptComponent: lists the scripts attached to an entity, lets one be added from
     * every CEntityScript subclass in the project, and draws each script's own properties.
     *
     * Two things fall out of the unification rather than being built here:
     *  - the add picker enumerates CClasses deriving CEntityScript, so C++ scripts appear alongside C# ones
     *    with no separate path;
     *  - a script's properties are real FPropertys on its (minted) class, so each one is drawn by a stock
     *    nested FPropertyTable instead of a bespoke drawer over a value blob.
     */
    class FEntityScriptComponentCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FEntityScriptComponentCustomization> MakeInstance();

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        // Per-script value editor. Rebuilt ONLY when that slot's script object or its class changes -- never
        // per frame, or every widget inside it loses its in-progress state and nothing can be edited.
        struct FSlotView
        {
            CEntityScript*             BoundScript = nullptr;
            const CClass*              BoundClass = nullptr;   // guards an object replaced at the same address
            TUniquePtr<FPropertyTable> ValueTable;
        };

        // What the nested tables reported this frame, translated into this customization's own return value so
        // an edit to a script property reaches the scene's transaction/dirty pipeline like any other property.
        EPropertyChangeOp NestedChangeOp = EPropertyChangeOp::None;

        // Captured during the draw and replayed from UpdatePropertyValue (after BeginTransaction), so the
        // undo snapshot is taken BEFORE the mutation. Mutating mid-draw would also invalidate the vector the
        // draw loop is walking.
        TFunction<void()> PendingMutation;
        bool              bFinishPending = false;

        TVector<FSlotView> SlotViews;
    };
}
