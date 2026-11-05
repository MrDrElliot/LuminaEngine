#pragma once

#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "GUID/GUID.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObject;

    /**
     * Retargets references to a set of objects, across the whole object graph and every registered reference
     * provider.
     *
     * Walks the reflected shape rather than re-serializing each object, which is what lets it reach a
     * NoSerialize property and a holder that is not a CObject at all. A null replacement clears the
     * reference instead of repointing it.
     */
    class RUNTIME_API FObjectReferenceReplacer
    {
    public:

        FObjectReferenceReplacer() = default;
        FObjectReferenceReplacer(CObject* ToReplace, CObject* Replacement);

        ~FObjectReferenceReplacer() = default;
        LE_NO_COPYMOVE(FObjectReferenceReplacer);

        /** Hard reference: matched by pointer. */
        void AddReplacement(CObject* ToReplace, CObject* Replacement);

        /** Soft reference: never loads its target, so it is matched on GUID or path instead. */
        void AddSoftReplacement(const FGuid& ToReplaceGUID, FStringView ToReplacePath,
            const FGuid& ReplacementGUID, FStringView ReplacementPath);

        NODISCARD bool IsEmpty() const { return HardEntries.IsEmpty() && SoftEntries.IsEmpty(); }

        /** Applies to every live object and every registered provider. Returns the number rewritten. */
        uint32 ApplyToAllObjects();

        /** Applies to one object's own properties only. */
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
