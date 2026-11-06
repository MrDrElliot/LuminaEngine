#pragma once

#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "GUID/GUID.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObject;

    // Walks reflected shape rather than re-serializing, so it reaches NoSerialize properties and non-CObject holders.
    class RUNTIME_API FObjectReferenceReplacer
    {
    public:

        FObjectReferenceReplacer() = default;
        FObjectReferenceReplacer(CObject* ToReplace, CObject* Replacement);

        ~FObjectReferenceReplacer() = default;
        LE_NO_COPYMOVE(FObjectReferenceReplacer);

        // Hard references are matched by pointer, and a null replacement clears them.
        void AddReplacement(CObject* ToReplace, CObject* Replacement);

        // Soft references never load their target, so they are matched on GUID or path instead.
        void AddSoftReplacement(const FGuid& ToReplaceGUID, FStringView ToReplacePath,
            const FGuid& ReplacementGUID, FStringView ReplacementPath);

        NODISCARD bool IsEmpty() const { return HardEntries.IsEmpty() && SoftEntries.IsEmpty(); }

        // Applies to every live object and every registered provider, returning the number rewritten.
        uint32 ApplyToAllObjects();

        // Applies to one object's own properties only.
        uint32 ApplyTo(CObject* Object);

        NODISCARD uint32 GetNumReplaced() const { return NumReplaced; }

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

        TVector<FHardEntry> HardEntries;
        TVector<FSoftEntry> SoftEntries;
        uint32              NumReplaced = 0;
    };
}
