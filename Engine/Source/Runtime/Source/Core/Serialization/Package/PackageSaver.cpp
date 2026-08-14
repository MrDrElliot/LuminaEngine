#include "RuntimePCH.h"
#include "PackageSaver.h"

#include "Core/Object/Package/Package.h"

namespace Lumina
{
    bool FSaveContext::AddExport(CObject* Export)
    {
        // An object whose refcount hit 0 mid-save is marked-destroy but not yet freed (e.g. an intermediate
        // created during a large import); it still matches the package, so GetObjectsWithPackage hands it back,
        // but its weak handle resolves to null at write time -> the WriteExports invariant trips. It is on its
        // way out, so skip it and don't recurse into a dying object's references.
        if (Export == nullptr || Export->HasAnyFlag(OF_MarkedDestroy))
        {
            return false;
        }

        if (!SeenExports.insert(Export).second)
        {
            return false;
        }

        Exports.push_back(Export);
        return true;
    }

    FArchive& FSaveReferenceBuilderArchive::operator<<(CObject*& Value)
    {
        if (Value == nullptr || Value->GetPackage() == nullptr)
        {
            return *this;
        }

        // Only recurse into same-package exports, and only on first sight.
        // Skipping the guard re-enters Serialize for shared/cyclic refs and infinite-loops.
        if (Value->GetPackage() == SaveContext->CurrentPackage)
        {
            if (SaveContext->AddExport(Value))
            {
                Value->Serialize(*this);
            }
        }

        return *this;
    }

    FArchive& FSaveReferenceBuilderArchive::operator<<(FObjectHandle& Value)
    {
        if (CObject* Object = Value.Resolve())
        {
            return FSaveReferenceBuilderArchive::operator<<(Object);
        }

        return *this;
    }

    FArchive& FPackageSaver::operator<<(CObject*& Value)
    {
        FObjectPackageIndex Index;
        // A reference to a dying object (skipped from the export table) would otherwise write a stale index;
        // serialize it as null instead. The default-constructed Index is the null sentinel.
        if (Value && !Value->HasAnyFlag(OF_MarkedDestroy))
        {
            if (Value->GetPackage() == Package)
            {
                Index = FObjectPackageIndex(Value->GetLoaderIndex());
            }
            else
            {
                if (ObjectToIndexMap.find(Value) != ObjectToIndexMap.end())
                {
                    Index = FObjectPackageIndex::FromImport(ObjectToIndexMap[Value]);
                }
                else
                {
                    Index = FObjectPackageIndex::FromImport(CurrentImportIndex);
                    ObjectToIndexMap.emplace(Value, CurrentImportIndex);
                    CurrentImportIndex++;
                }
            }
        }
        
        *this << Index;
        
        return *this;
    }

    FArchive& FPackageSaver::operator<<(FObjectHandle& Value)
    {
        FObjectPackageIndex Index;
        if (CObject* Obj = Value.Resolve(); Obj && !Obj->HasAnyFlag(OF_MarkedDestroy))
        {
            if (Obj->GetPackage() == Package)
            {
                Index = FObjectPackageIndex(Obj->GetLoaderIndex());
            }
            else
            {
                if (ObjectToIndexMap.find(Obj) != ObjectToIndexMap.end())
                {
                    Index = FObjectPackageIndex::FromImport(ObjectToIndexMap[Obj]);
                }
                else
                {
                    Index = FObjectPackageIndex::FromImport(CurrentImportIndex);
                    ObjectToIndexMap.emplace(Obj, CurrentImportIndex);
                    CurrentImportIndex++;
                }
            }
        }

        *this << Index;

        return *this;
    }

    void FPackageSaver::RegisterSoftAssetReference(const FGuid& AssetGUID)
    {
        if (!AssetGUID.IsValid())
        {
            return;
        }
        SoftReferencedGUIDs.insert(AssetGUID);
    }

    bool FPackageSaver::WriteBulkData(FBulkDataRef& OutRef, const void* Data, int64 Size)
    {
        // Passthrough: the region this ref points into is being copied to the new file byte for byte, so
        // the payload is already exactly where OutRef says it is. Succeeding without touching either is
        // the whole mechanism -- it is what lets a rename move a multi-megabyte region it never read.
        if (IsBulkPassthrough())
        {
            if (OutRef.IsValid())
            {
                return true;
            }

            // A payload with no ref into the region being copied. Passthrough has nowhere to put it, and
            // the alternative -- writing the null ref through -- is the payload gone. Say so; the saver
            // reads this as "these bytes cannot be spliced" and rebuilds the region the long way instead.
            if (Package != nullptr)
            {
                Package->FlagUnresolvedBulkData();
            }
            return false;
        }

        if (Data == nullptr || Size <= 0)
        {
            OutRef = FBulkDataRef{};
            return false;
        }

        OutRef.Offset = (int64)BulkBytes.size();
        OutRef.Size   = Size;

        BulkBytes.resize(BulkBytes.size() + (size_t)Size);
        Memory::Memcpy(BulkBytes.data() + (size_t)OutRef.Offset, Data, (size_t)Size);

        return true;
    }

    void FPackageSaver::PopulateImportTable(TVector<FObjectImport>& Out) const
    {
        Out.clear();

        // Hard imports keep their write-time slots so FObjectPackageIndex refs stay valid.
        Out.resize(CurrentImportIndex);
        THashSet<FGuid> HardGUIDs;
        HardGUIDs.reserve(ObjectToIndexMap.size());
        for (const auto& [Obj, Idx] : ObjectToIndexMap)
        {
            FObjectImport Entry(Obj);
            Entry.Type = EDependencyType::Hard;
            HardGUIDs.insert(Entry.ObjectGUID);
            Out[Idx] = Move(Entry);
        }

        // Append soft imports not already hard (no data-stream index; exist only as a typed
        // edge for AssetRegistry/FCookGraph). Sort first: hash_set order would break determinism.
        TVector<FGuid> SortedSoft;
        SortedSoft.reserve(SoftReferencedGUIDs.size());
        for (const FGuid& Guid : SoftReferencedGUIDs)
        {
            if (HardGUIDs.find(Guid) != HardGUIDs.end()) continue;
            SortedSoft.push_back(Guid);
        }
        eastl::sort(SortedSoft.begin(), SortedSoft.end());
        for (const FGuid& Guid : SortedSoft)
        {
            Out.emplace_back(Guid, EDependencyType::Soft);
        }
    }
}
