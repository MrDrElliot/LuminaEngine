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

        HardEntries.Emplace(ToReplace, Replacement);
    }

    void FObjectReferenceReplacer::AddSoftReplacement(const FGuid& ToReplaceGUID, FStringView ToReplacePath,
        const FGuid& ReplacementGUID, FStringView ReplacementPath)
    {
        if (!ToReplaceGUID.IsValid() && ToReplacePath.IsEmpty())
        {
            return;
        }

        FSoftEntry Entry;
        Entry.ToReplaceGUID = ToReplaceGUID;
        Entry.ToReplacePath = FStringView(ToReplacePath.GetData(), ToReplacePath.Len());
        Entry.ReplacementGUID = ReplacementGUID;
        Entry.ReplacementPath = FStringView(ReplacementPath.GetData(), ReplacementPath.Len());

        SoftEntries.Add(Move(Entry));
    }

    CObject* FObjectReferenceReplacer::MatchHard(CObject* Current, TVector<TStrongObjectPtr<CObject>>& Released)
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
                if (Object::Internal::GetStrongRefCountByIndex(Current->GetInternalIndex()) > 0)
                {
                    Released.Emplace(Current);
                }
                return Entry.Replacement;
            }
        }
        return Current;
    }

    uint32 FObjectReferenceReplacer::ApplyTo(CObject* Object)
    {
        if (Object == nullptr || IsEmpty())
        {
            return 0;
        }

        const uint32 Before = NumReplaced;

        // A swapped-out object is held until the walk ends, since its release can free the very object being walked.
        TVector<TStrongObjectPtr<CObject>> Released;
        auto Hard = [this, &Released](CObject* Current) -> CObject* { return MatchHard(Current, Released); };

        auto Soft = [this](FSoftObjectPath& Path)
        {
            for (const FSoftEntry& Entry : SoftEntries)
            {
                const bool bGUIDMatch = Entry.ToReplaceGUID.IsValid()
                    && Path.GetCachedGUID() == Entry.ToReplaceGUID;
                const bool bPathMatch = !Entry.ToReplacePath.IsEmpty()
                    && Path.GetPath() == FStringView(*Entry.ToReplacePath, Entry.ToReplacePath.Len());
                if (!bGUIDMatch && !bPathMatch)
                {
                    continue;
                }

                Path = FSoftObjectPath(
                    FStringView(*Entry.ReplacementPath, Entry.ReplacementPath.Len()),
                    Entry.ReplacementGUID);
                ++NumReplaced;
                return;
            }
        };

        // Clearing a reference can free Object itself through a cycle, such as a skeleton and its preview mesh owning each other.
        const FObjectHandle Handle = Object::Internal::GetHandleByObject(Object);
        auto IsAlive = [&Handle]() { return Object::Internal::ResolveHandle(Handle) != nullptr; };

        if (SoftEntries.IsEmpty())
        {
            FObjectReferenceVisitor::VisitStruct(Object->GetClass(), Object, Hard);
        }
        else
        {
            FObjectReferenceVisitor::VisitStructWithSoft(Object->GetClass(), Object, Hard, Soft);
        }

        if (IsAlive())
        {
            Object->VisitAdditionalObjectReferences(Hard);
        }

        const uint32 Replaced = NumReplaced - Before;
        if (Replaced > 0 && IsAlive())
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
        ForEachObject([&](CObjectBase* Base, int32)
        {
            if (Base != nullptr && !Base->HasAnyFlag(OF_MarkedDestroy))
            {
                Live.Add(Object::Internal::GetHandleByObject(Base));
            }
        });

        for (const FObjectHandle& Handle : Live)
        {
            if (CObjectBase* Object = Object::Internal::ResolveHandle(Handle))
            {
                ApplyTo(static_cast<CObject*>(Object));
            }
        }

        // The holders no walk of the object graph can reach, which is most of what an ECS world is.
        TVector<TStrongObjectPtr<CObject>> Released;
        auto Hard = [this, &Released](CObject* Current) -> CObject* { return MatchHard(Current, Released); };

        FObjectReferenceProviders::ForEach([&Hard](IObjectReferenceProvider& Provider)
        {
            Provider.VisitObjectReferences(Hard);
        });

        return NumReplaced - Before;
    }
}
