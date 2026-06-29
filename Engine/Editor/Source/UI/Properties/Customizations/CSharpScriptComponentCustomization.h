#pragma once

#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class CScriptStruct;
    class FPropertyTable;

    class FCSharpScriptComponentPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FCSharpScriptComponentPropertyCustomization> MakeInstance();

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        // Per-slot value editor, rebuilt when that slot's resolved layout or live buffer changes.
        struct FSlotView
        {
            const CScriptStruct*       BoundLayout = nullptr;
            void*                      BoundBuffer = nullptr;
            TUniquePtr<FPropertyTable> ValueTable;
        };

        // The pick is captured here and replayed from UpdatePropertyValue (after BeginTransaction) so
        // the undo snapshot is pre-change.
        TFunction<void()> PendingMutation;
        bool bFinishPending = false;

        // Parallel to the component's Scripts vector.
        TVector<FSlotView> SlotViews;
        bool               bValueEdited = false;
    };
}
