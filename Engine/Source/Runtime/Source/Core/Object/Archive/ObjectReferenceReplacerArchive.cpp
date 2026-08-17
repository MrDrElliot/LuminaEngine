#include "RuntimePCH.h"
#include "ObjectReferenceReplacerArchive.h"


namespace Lumina
{
    FObjectReferenceReplacerArchive::FObjectReferenceReplacerArchive()
    {
        SetFlag(EArchiverFlags::Writing);
    }

    FObjectReferenceReplacerArchive::FObjectReferenceReplacerArchive(CObject* InToReplace, CObject* InReplacement)
    {
        SetFlag(EArchiverFlags::Writing);
        AddReplacement(InToReplace, InReplacement);
    }

    FObjectReferenceReplacerArchive::~FObjectReferenceReplacerArchive()
    {
        for (CObject* Object : RootedObjects)
        {
            Object->RemoveFromRoot();
        }
    }

    void FObjectReferenceReplacerArchive::Root(CObject* Object)
    {
        if (Object == nullptr)
        {
            return;
        }

        for (CObject* Existing : RootedObjects)
        {
            if (Existing == Object)
            {
                return;
            }
        }

        Object->AddToRoot();
        RootedObjects.push_back(Object);
    }

    void FObjectReferenceReplacerArchive::AddReplacement(CObject* ToReplace, CObject* Replacement)
    {
        if (ToReplace == nullptr || ToReplace == Replacement)
        {
            return;
        }

        Root(ToReplace);
        Root(Replacement);

        HardEntries.push_back(FHardEntry{ ToReplace, Replacement });
    }

    void FObjectReferenceReplacerArchive::AddSoftReplacement(const FGuid& ToReplaceGUID, FStringView ToReplacePath, const FGuid& ReplacementGUID, FStringView ReplacementPath)
    {
        if (!ToReplaceGUID.IsValid() && ToReplacePath.empty())
        {
            return;
        }

        FSoftEntry Entry;
        Entry.ToReplaceGUID   = ToReplaceGUID;
        Entry.ToReplacePath.assign(ToReplacePath.data(), ToReplacePath.size());
        Entry.ReplacementGUID = ReplacementGUID;
        Entry.ReplacementPath.assign(ReplacementPath.data(), ReplacementPath.size());

        SoftEntries.push_back(Move(Entry));
    }

    FArchive& FObjectReferenceReplacerArchive::operator<<(CObject*& Obj)
    {
        if (Obj == nullptr)
        {
            return *this;
        }

        for (const FHardEntry& Entry : HardEntries)
        {
            if (Entry.ToReplace == Obj)
            {
                Obj = Entry.Replacement;
                ++NumReplaced;
                break;
            }
        }

        return *this;
    }

    FArchive& FObjectReferenceReplacerArchive::operator<<(FObjectHandle& Handle)
    {
        CObject* Resolved = Handle.Resolve();
        if (Resolved == nullptr)
        {
            return *this;
        }

        for (const FHardEntry& Entry : HardEntries)
        {
            if (Entry.ToReplace == Resolved)
            {
                Handle = Entry.Replacement ? FObjectHandle(Entry.Replacement) : FObjectHandle();
                ++NumReplaced;
                break;
            }
        }

        return *this;
    }

    bool FObjectReferenceReplacerArchive::RewriteSoftAssetReference(FString& Path, FGuid& AssetGUID)
    {
        for (const FSoftEntry& Entry : SoftEntries)
        {
            const bool bGUIDMatch = Entry.ToReplaceGUID.IsValid() && AssetGUID == Entry.ToReplaceGUID;
            const bool bPathMatch = !Entry.ToReplacePath.empty() && Path == Entry.ToReplacePath;
            if (!bGUIDMatch && !bPathMatch)
            {
                continue;
            }

            Path      = Entry.ReplacementPath;
            AssetGUID = Entry.ReplacementGUID;
            ++NumReplaced;
            return true;
        }

        return false;
    }
}
