#include "RuntimePCH.h"
#include "PackageLoader.h"

#include "Core/Object/ObjectArray.h"
#include "Core/Object/Package/Package.h"
#include "Core/Versioning/CoreVersion.h"
#include "Core/Serialization/PortableBytes.h"
#include "Memory/Memcpy.h"

namespace Lumina
{
    FArchive& FPackageLoader::operator<<(CObject*& Value)
    {
        FObjectPackageIndex Index;
        FArchive& Ar = *this;
        Ar << Index;

        FLoadRequest Standalone;
        Value = Package->IndexToObject(Index, Request != nullptr ? *Request : Standalone);

        return Ar;
    }

    bool FPackageLoader::DescribeCutBytes(FPortableBytes& Cut) const
    {
        if (Package == nullptr)
        {
            return false;
        }

        const bool bNameSlots = Names != nullptr && Cut.FileVersion >= (int32)ELuminaEngineVersion::PACKAGE_NAME_TABLE;
        const int32 NumImports = Package->ImportTable.Num();
        const int32 NumExports = Package->ExportTable.Num();

        THashSet<uint32> SeenNames;
        THashSet<int32> SeenObjects;
        for (size_t Offset = 0; Offset + sizeof(uint32) <= static_cast<size_t>(Cut.Bytes.Num()); ++Offset)
        {
            uint32 Word = 0;
            Memory::Memcpy(&Word, Cut.Bytes.GetData() + Offset, sizeof(uint32));

            const uint32 Slot = Word & kPackageNameSlotMask;
            if (bNameSlots && Slot < static_cast<uint32>(Names->Num()) && SeenNames.TryAdd(Slot))
            {
                Cut.Names.Emplace(Slot, Names->Resolve(Slot, false, 0));
            }

            const FObjectPackageIndex Index((int32)Word);
            const int64 Raw = Index.GetRaw();
            if (Raw == 0 || Raw < -(int64)NumImports || Raw > (int64)NumExports || !SeenObjects.TryAdd(Index.GetRaw()))
            {
                continue;
            }
            const FGuid& Object = Index.IsImport()
                ? Package->ImportTable[Index.GetArrayIndex()].ObjectGUID
                : Package->ExportTable[Index.GetArrayIndex()].ObjectGUID;
            Cut.Objects.Emplace(Index.GetRaw(), Object);
        }
        return true;
    }

    FArchive& FPackageLoader::operator<<(FName& Value)
    {
        if (Names == nullptr || GetFileVersion() < (int32)ELuminaEngineVersion::PACKAGE_NAME_TABLE)
        {
            return FArchive::operator<<(Value);
        }

        SerializePackageName(*this, Value, nullptr, Names.get());
        return *this;
    }

    FArchive& FPackageLoader::operator<<(FObjectHandle& Value)
    {
        FObjectPackageIndex Index;
        FArchive& Ar = *this;
        Ar << Index;

        FLoadRequest  Standalone;
        FLoadRequest& Active = Request != nullptr ? *Request : Standalone;
        CObject* Object = Package->IndexToObject(Index, Active);
        Value = Object::Internal::GetHandleByObject(Object);
        Package->LoadObject(Object, Active);

        return Ar;
    }
}
