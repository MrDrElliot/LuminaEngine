#include "RuntimePCH.h"
#include "PackageSaver.h"

#include "Core/Object/Package/Package.h"

namespace Lumina
{
    bool FSaveContext::AddExport(CObject* Export)
    {
        // A dying object's weak handle resolves to null at write time and trips the WriteExports invariant.
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

        // Without the guard a shared or cyclic ref re-enters Serialize and loops forever.
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
        // A skipped dying object would otherwise write a stale index, so serialize it as null.
        if (Value && !Value->HasAnyFlag(OF_MarkedDestroy) && Value->GetPackage() != nullptr)
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
        // An import resolves by GUID against a package, so a package-less write could only dangle.
        if (CObject* Obj = Value.Resolve(); Obj && !Obj->HasAnyFlag(OF_MarkedDestroy) && Obj->GetPackage() != nullptr)
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
        // The payload is already exactly where the ref says, which is what lets a rename move a huge region.
        if (IsBulkPassthrough())
        {
            if (OutRef.IsValid())
            {
                return true;
            }

            // The saver reads this as unspliceable and rebuilds the region the long way instead.
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

    void FPackageSaver::FlagUnresolvedBulkData()
    {
        if (Package != nullptr)
        {
            Package->FlagUnresolvedBulkData();
        }
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

        // Sorted first, since hash-set order would break determinism.
        TVector<FGuid> SortedSoft;
        SortedSoft.reserve(SoftReferencedGUIDs.size());
        for (const FGuid& Guid : SoftReferencedGUIDs)
        {
            if (HardGUIDs.find(Guid) != HardGUIDs.end()) continue;
            SortedSoft.push_back(Guid);
        }
        Algo::Sort(SortedSoft.begin(), SortedSoft.end());
        for (const FGuid& Guid : SortedSoft)
        {
            Out.emplace_back(Guid, EDependencyType::Soft);
        }
    }
}
