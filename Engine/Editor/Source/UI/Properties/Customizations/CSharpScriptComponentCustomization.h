#pragma once

#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
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

        // The pick is captured here and replayed from UpdatePropertyValue (after BeginTransaction) so
        // the undo snapshot is pre-change.
        TFunction<void()> PendingMutation;
        bool bFinishPending = false;

        // Rebuilt when the resolved layout or live buffer changes.
        TUniquePtr<FPropertyTable> ValueTable;
        const CScriptStruct*       BoundLayout = nullptr;
        void*                      BoundBuffer = nullptr;
        bool                       bValueEdited = false;
    };
}
