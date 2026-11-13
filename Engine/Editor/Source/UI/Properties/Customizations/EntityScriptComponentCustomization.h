#pragma once

#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/String.h"
#include "Memory/SmartPtr.h"
#include "UI/Widgets/CategorizedPicker.h"

namespace Lumina
{
    class CEntityScript;
    struct SEntityScriptComponent;
    class FFunction;
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
     *
     * A function with Button metadata, C++ FUNCTION(Button) or C# [Function(Button)], is drawn under those properties and called through reflection.
     */
    class FEntityScriptComponentCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FEntityScriptComponentCustomization> MakeInstance();

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;
        bool ApplySearch(const TSharedPtr<FPropertyHandle>& Property, FStringView Text) override;

    private:

        // Queues moving one script to another slot, which is also the order they attach, ready and update in.
        void QueueMove(SEntityScriptComponent* Component, int32 From, int32 To);

        // Per-script value editor. Rebuilt ONLY when that slot's script object or its class changes -- never
        // per frame, or every widget inside it loses its in-progress state and nothing can be edited.
        struct FSlotView
        {
            CEntityScript*             BoundScript = nullptr;
            const CClass*              BoundClass = nullptr;   // guards an object replaced at the same address
            TUniquePtr<FPropertyTable> ValueTable;

            // The class's functions that carry Button metadata, gathered with the table rather than per frame.
            TVector<const FFunction*> Buttons;

            // Resolved with the rest of the binding, since it walks the script directories off disk.
            FString SourceFile;

            bool bPassesSearch = true;
        };

        void BindView(FSlotView& View, CEntityScript* Script);

        // What the nested tables reported this frame, translated into this customization's own return value so
        // an edit to a script property reaches the scene's transaction/dirty pipeline like any other property.
        EPropertyChangeOp NestedChangeOp = EPropertyChangeOp::None;

        // Captured during the draw and replayed from UpdatePropertyValue (after BeginTransaction), so the
        // undo snapshot is taken BEFORE the mutation. Mutating mid-draw would also invalidate the vector the
        // draw loop is walking.
        TFunction<void()> PendingMutation;

        // Owned here, so its search and open categories last as long as this inspector does.
        FCategorizedPicker ScriptPicker;
        bool              bFinishPending = false;

        TVector<FSlotView> SlotViews;
    };
}
