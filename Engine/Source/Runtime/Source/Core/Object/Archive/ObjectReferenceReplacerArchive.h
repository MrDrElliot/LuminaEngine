#pragma once
#include "Core/Object/Object.h"

namespace Lumina
{
    class FObjectReferenceReplacerArchive : public FArchive
    {
    public:
        
        LE_NO_COPY(FObjectReferenceReplacerArchive);

        FObjectReferenceReplacerArchive(CObject* InToReplace, CObject* InReplacement)
            : ToReplace(InToReplace)
            , Replacement(InReplacement)
        {
            SetFlag(EArchiverFlags::Writing);
            
            // Ensure the replacement does not get destroyed.
            if (Replacement)
            {
                Replacement->AddToRoot();
            }

            InToReplace->AddToRoot();
        }

        ~FObjectReferenceReplacerArchive() override
        {
            if (Replacement)
            {
                Replacement->RemoveFromRoot();
            }

            ToReplace->RemoveFromRoot();
        }


        RUNTIME_API FArchive& operator<<(CObject*& Obj) override;

    private:

        CObject* ToReplace;
        CObject* Replacement;
        
    };
}
