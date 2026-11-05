#include "RuntimePCH.h"
#include "PackageSaver.h"

#include "Core/Object/Package/Package.h"
#include "Core/Versioning/CoreVersion.h"

namespace Lumina
{
    bool FSaveContext::AddExport(CObject* Export)
    {
        // A dying object's weak handle resolves to null at write time and trips the WriteExports invariant.
        if (Export == nullptr || Export->HasAnyFlag(OF_MarkedDestroy))
        {
            return false;
        }

        if (ExcludeExport && ExcludeExport(Export))
        {
            return false;
        }

        if (!SeenExports.TryAdd(Export))
        {
            return false;
        }

        Exports.Add(Export);
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

    FArchive& FPackageSaver::operator<<(FName& Value)
    {
        // mirrors FPackageLoader, so a version-pinned save round-trips through the path that reads it
        if (GetFileVersion() < (int32)ELuminaEngineVersion::PACKAGE_NAME_TABLE)
        {
            return FArchive::operator<<(Value);
        }

        SerializePackageName(*this, Value, &NameMap, nullptr);
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
                if (SavedExports == nullptr || SavedExports->Contains(Value))
                {
                    Index = FObjectPackageIndex((int32)Value->GetLoaderIndex());
                }
            }
            else
            {
                if (const uint32* Existing = ObjectToIndexMap.Find(Value))
                {
                    Index = FObjectPackageIndex::FromImport(*Existing);
                }
                else
                {
                    Index = FObjectPackageIndex::FromImport(CurrentImportIndex);
                    ObjectToIndexMap.TryAdd(Value, CurrentImportIndex);
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
                if (SavedExports == nullptr || SavedExports->Contains(Obj))
                {
                    Index = FObjectPackageIndex((int32)Obj->GetLoaderIndex());
                }
            }
            else
            {
                if (const uint32* Existing = ObjectToIndexMap.Find(Obj))
                {
                    Index = FObjectPackageIndex::FromImport(*Existing);
                }
                else
                {
                    Index = FObjectPackageIndex::FromImport(CurrentImportIndex);
                    ObjectToIndexMap.TryAdd(Obj, CurrentImportIndex);
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
        SoftReferencedGUIDs.Add(AssetGUID);

        for (FStringView Rest = CurrentBundles; !Rest.IsEmpty();)
        {
            const int32 Comma = Rest.Find(',');
            FStringView Name = Comma == Constants::kIndexNone ? Rest : Rest.Left(Comma);
            Rest = Comma == Constants::kIndexNone ? FStringView() : Rest.Mid(Comma + 1);
            const int32 FirstChar = Name.FindFirstNotOf(" ");
            Name.RemovePrefix(FirstChar == Constants::kIndexNone ? Name.Len() : FirstChar);
            Name.RemoveSuffix(Name.Len() - Math::Min(Name.FindLastNotOf(" ") + 1, Name.Len()));
            const FName Bundle = Name.IsEmpty() ? FName() : FName(Name);
            TVector<FName>& Bundles = SoftReferenceBundles[AssetGUID];
            if (!Bundle.IsNone() && !Algo::Contains(Bundles, Bundle))
            {
                Bundles.Add(Bundle);
            }
        }
    }

    FStringView FPackageSaver::SetSoftReferenceBundles(FStringView Bundles)
    {
        const FStringView Previous = CurrentBundles;
        CurrentBundles = Bundles;
        return Previous;
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

        OutRef.Offset = BulkBytes.Num();
        OutRef.Size   = Size;

        BulkBytes.SetNum(IntCastChecked<int32>(BulkBytes.Num() + (size_t)Size));
        Memory::Memcpy(BulkBytes.GetData() + (size_t)OutRef.Offset, Data, (size_t)Size);

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
        Out.Reset();

        // Hard imports keep their write-time slots so FObjectPackageIndex refs stay valid.
        Out.SetNum(CurrentImportIndex);
        THashSet<FGuid> HardGUIDs;
        HardGUIDs.Reserve(ObjectToIndexMap.Num());
        for (const auto& [Obj, Idx] : ObjectToIndexMap)
        {
            FObjectImport Entry(Obj);
            Entry.Type = EDependencyType::Hard;
            HardGUIDs.Add(Entry.ObjectGUID);
            Out[Idx] = Move(Entry);
        }

        // Sorted first, since hash-set order would break determinism.
        TVector<FGuid> SortedSoft;
        SortedSoft.Reserve(SoftReferencedGUIDs.Num());
        for (const FGuid& Guid : SoftReferencedGUIDs)
        {
            if (HardGUIDs.Contains(Guid)) continue;
            SortedSoft.Add(Guid);
        }
        Algo::Sort(SortedSoft);
        for (const FGuid& Guid : SortedSoft)
        {
            Out.Emplace(Guid, EDependencyType::Soft);
        }

        for (FObjectImport& Import : Out)
        {
            if (auto It = SoftReferenceBundles.Find(Import.ObjectGUID); It != nullptr)
            {
                Import.Bundles = (*It);
            }
        }
    }
}
