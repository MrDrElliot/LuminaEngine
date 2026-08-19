#pragma once
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Object/Object.h"
#include "Core/Object/SoftObjectPtr.h"
#include "GUID/GUID.h"

namespace Lumina
{
    // Retargets references to a set of objects as an object graph is walked with Serialize().
    class FObjectReferenceReplacerArchive : public FArchive
    {
    public:

        using FArchive::operator<<;

        LE_NO_COPY(FObjectReferenceReplacerArchive);

        RUNTIME_API FObjectReferenceReplacerArchive();

        RUNTIME_API FObjectReferenceReplacerArchive(CObject* InToReplace, CObject* InReplacement);

        RUNTIME_API ~FObjectReferenceReplacerArchive() override;

        // A null Replacement clears the reference.
        RUNTIME_API void AddReplacement(CObject* ToReplace, CObject* Replacement);

        // Soft refs never load their target, so they are matched on GUID and path rather than pointer.
        RUNTIME_API void AddSoftReplacement(const FGuid& ToReplaceGUID, FStringView ToReplacePath, const FGuid& ReplacementGUID, FStringView ReplacementPath);

        RUNTIME_API FArchive& operator<<(CObject*& Obj) override;
        RUNTIME_API FArchive& operator<<(FObjectHandle& Handle) override;
        RUNTIME_API bool RewriteSoftAssetReference(FString& Path, FGuid& AssetGUID) override;

        uint32 GetNumReplaced() const { return NumReplaced; }
        void ResetNumReplaced() { NumReplaced = 0; }

    private:

        struct FHardEntry
        {
            CObject* ToReplace   = nullptr;
            CObject* Replacement = nullptr;
        };

        struct FSoftEntry
        {
            FGuid   ToReplaceGUID;
            FString ToReplacePath;
            FGuid   ReplacementGUID;
            FString ReplacementPath;
        };

        // Pinned for the archive's lifetime so a swap cannot free an object mid-walk.
        void Root(CObject* Object);

        TVector<FHardEntry>  HardEntries;
        TVector<FSoftEntry>  SoftEntries;
        TVector<CObject*>    RootedObjects;
        uint32               NumReplaced = 0;
    };
}
