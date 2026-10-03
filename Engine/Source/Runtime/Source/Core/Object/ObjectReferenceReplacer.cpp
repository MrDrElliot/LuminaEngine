#include "RuntimePCH.h"

#include "ObjectReferenceReplacer.h"

#include "Class.h"
#include "Object.h"
#include "ObjectArray.h"
#include "ObjectHandleTyped.h"
#include "ObjectReferenceProvider.h"
#include "SoftObjectPtr.h"
#include "Core/Reflection/Type/ObjectReferenceVisitor.h"

namespace Lumina
{
    FObjectReferenceReplacer::FObjectReferenceReplacer(CObject* ToReplace, CObject* Replacement)
    {
        AddReplacement(ToReplace, Replacement);
    }

    void FObjectReferenceReplacer::AddReplacement(CObject* ToReplace, CObject* Replacement)
    {
        if (ToReplace == nullptr || ToReplace == Replacement)
        {
            return;
        }

        HardEntries.push_back(FHardEntry{ ToReplace, Replacement });
    }

    void FObjectReferenceReplacer::AddSoftReplacement(const FGuid& ToReplaceGUID, FStringView ToReplacePath,
        const FGuid& ReplacementGUID, FStringView ReplacementPath)
    {
        if (!ToReplaceGUID.IsValid() && ToReplacePath.empty())
        {
            return;
        }

        FSoftEntry Entry;
        Entry.ToReplaceGUID = ToReplaceGUID;
        Entry.ToReplacePath.assign(ToReplacePath.data(), ToReplacePath.size());
        Entry.ReplacementGUID = ReplacementGUID;
        Entry.ReplacementPath.assign(ReplacementPath.data(), ReplacementPath.size());

        SoftEntries.push_back(Move(Entry));
    }

    uint32 FObjectReferenceReplacer::ApplyTo(CObject* Object)
    {
        if (Object == nullptr || IsEmpty())
        {
            return 0;
        }

        const uint32 Before = NumReplaced;

        auto Hard = [this](CObject* Current) -> CObject*
        {
            if (Current == nullptr)
            {
                return nullptr;
            }
            for (const FHardEntry& Entry : HardEntries)
            {
                if (Entry.ToReplace == Current)
                {
                    ++NumReplaced;
                    return Entry.Replacement;
                }
            }
            return Current;
        };

        auto Soft = [this](FSoftObjectPath& Path)
        {
            for (const FSoftEntry& Entry : SoftEntries)
            {
                const bool bGUIDMatch = Entry.ToReplaceGUID.IsValid()
                    && Path.GetCachedGUID() == Entry.ToReplaceGUID;
                const bool bPathMatch = !Entry.ToReplacePath.empty()
                    && Path.GetPath() == FStringView(Entry.ToReplacePath.c_str(), Entry.ToReplacePath.size());
                if (!bGUIDMatch && !bPathMatch)
                {
                    continue;
                }

                Path = FSoftObjectPath(
                    FStringView(Entry.ReplacementPath.c_str(), Entry.ReplacementPath.size()),
                    Entry.ReplacementGUID);
                ++NumReplaced;
                return;
            }
        };

        if (SoftEntries.empty())
        {
            FObjectReferenceVisitor::VisitStruct(Object->GetClass(), Object, Hard);
        }
        else
        {
            FObjectReferenceVisitor::VisitStructWithSoft(Object->GetClass(), Object, Hard, Soft);
        }

        Object->VisitAdditionalObjectReferences(Hard);

        const uint32 Replaced = NumReplaced - Before;
        if (Replaced > 0)
        {
            Object->OnReferencesReplaced();
        }
        return Replaced;
    }

    uint32 FObjectReferenceReplacer::ApplyToAllObjects()
    {
        if (IsEmpty())
        {
            return 0;
        }

        const uint32 Before = NumReplaced;

        // Handles, not pointers: clearing a reference can drop an object's last one and free it mid-pass, and
        // a handle resolves to null once that happens instead of handing back freed memory. Taking a strong
        // reference instead would be worse, since releasing one an object never had destroys it outright.
        TVector<FObjectHandle> Live;
        GObjectArray.ForEachObject([&](CObjectBase* Base, int32)
        {
            if (Base != nullptr && !Base->HasAnyFlag(OF_MarkedDestroy))
            {
                Live.push_back(GObjectArray.GetHandleByObject(Base));
            }
        });

        for (const FObjectHandle& Handle : Live)
        {
            if (CObjectBase* Object = GObjectArray.ResolveHandle(Handle))
            {
                ApplyTo(static_cast<CObject*>(Object));
            }
        }

        // The holders no walk of the object graph can reach, which is most of what an ECS world is.
        auto Hard = [this](CObject* Current) -> CObject*
        {
            if (Current == nullptr)
            {
                return nullptr;
            }
            for (const FHardEntry& Entry : HardEntries)
            {
                if (Entry.ToReplace == Current)
                {
                    ++NumReplaced;
                    return Entry.Replacement;
                }
            }
            return Current;
        };

        FObjectReferenceProviders::ForEach([&Hard](IObjectReferenceProvider& Provider)
        {
            Provider.VisitObjectReferences(Hard);
        });

        return NumReplaced - Before;
    }
}
