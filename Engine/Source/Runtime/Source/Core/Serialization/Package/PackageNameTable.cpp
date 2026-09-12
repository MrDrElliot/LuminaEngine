#include "RuntimePCH.h"
#include "PackageNameTable.h"

#include "Core/Assertions/Assert.h"
#include "Core/Versioning/CoreVersion.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        constexpr uint32 kHasNumberBit = 0x80000000u;
        constexpr uint32 kSlotMask     = 0x7FFFFFFFu;
    }

    uint32 FPackageNameMap::Slot(const FName& Name)
    {
        const FName Base = Name.GetBaseName();
        const uint32 Key = Base.GetID();

        if (const auto It = BaseToSlot.find(Key); It != BaseToSlot.end())
        {
            return It->second;
        }

        const uint32 NewSlot = (uint32)Slots.size();
        DEBUG_ASSERT(NewSlot < kHasNumberBit);
        BaseToSlot.emplace(Key, NewSlot);
        Slots.push_back(Base);
        return NewSlot;
    }

    void FPackageNameMap::Serialize(FArchive& Ar) const
    {
        uint32 Count = (uint32)Slots.size();
        Ar << Count;

        // length then bytes, the encoding an FString writes, without building one per name
        for (const FName& Name : Slots)
        {
            // none renders as the text "NAME_None", so it goes on the wire empty, as FArchive spells it
            const bool  bNone  = Name.IsNone();
            const char* Text   = bNone ? "" : Name.c_str();
            uint64      Length = bNone ? 0 : strlen(Text);

            Ar << Length;
            if (Length)
            {
                Ar.Serialize(const_cast<char*>(Text), (int64)Length);
            }
        }
    }

    void FPackageNameTable::Serialize(FArchive& Ar)
    {
        Slots.clear();

        uint32 Count = 0;
        Ar << Count;

        if (Count > Ar.GetMaxSerializeSize())
        {
            LOG_ERROR("Package name table claims {} entries (max {}); the package is corrupt", Count, Ar.GetMaxSerializeSize());
            Ar.SetHasError(true);
            return;
        }

        Slots.reserve(Count);

        // straight into the stack and interned once, an FString here cost three allocator calls per name
        char Stack[512];

        for (uint32 Index = 0; Index < Count; ++Index)
        {
            uint64 Length = 0;
            Ar << Length;

            if (Length > Ar.GetMaxSerializeSize())
            {
                LOG_ERROR("Package name {} claims {} bytes; the name table is corrupt", Index, Length);
                Ar.SetHasError(true);
                Slots.clear();
                return;
            }

            if (Length == 0)
            {
                Slots.push_back(NAME_None);
                continue;
            }

            // checked before interning, or a corrupt file would leave junk in the process-wide name table
            if (Length < sizeof(Stack))
            {
                Ar.Serialize(Stack, (int64)Length);
                if (Ar.HasError())
                {
                    Slots.clear();
                    return;
                }

                Stack[Length] = '\0';
                Slots.push_back(FName(Stack));
            }
            else
            {
                FString Heap;
                Heap.resize(Length);
                Ar.Serialize(Heap.data(), (int64)Length);
                if (Ar.HasError())
                {
                    Slots.clear();
                    return;
                }

                Slots.push_back(FName(Heap));
            }
        }
    }

    FName FPackageNameTable::Resolve(uint32 Slot, bool bHasNumber, uint32 ExternalNumber) const
    {
        if (Slot >= Slots.size())
        {
            LOG_ERROR("Package name slot {} is outside the {}-entry name table", Slot, Slots.size());
            return NAME_None;
        }

        const FName Base = Slots[Slot];
        return bHasNumber ? FName::FromIndexAndNumber(Base.GetID(), ExternalNumber) : Base;
    }

    void SerializePackageName(FArchive& Ar, FName& Value, FPackageNameMap* Map, const FPackageNameTable* Table)
    {
        if (Ar.IsWriting())
        {
            DEBUG_ASSERT(Map != nullptr);

            uint32 Packed = Map->Slot(Value);
            if (Value.HasNumber())
            {
                Packed |= kHasNumberBit;
                uint32 Number = Value.GetNumber();
                Ar << Packed;
                Ar << Number;
            }
            else
            {
                Ar << Packed;
            }
            return;
        }

        DEBUG_ASSERT(Table != nullptr);

        uint32 Packed = 0;
        Ar << Packed;

        const bool bHasNumber = (Packed & kHasNumberBit) != 0;
        uint32 Number = 0;
        if (bHasNumber)
        {
            Ar << Number;
        }

        Value = Table->Resolve(Packed & kSlotMask, bHasNumber, Number);
    }

    FArchive& FPackageContainerReader::operator<<(FName& Value)
    {
        if (Names == nullptr || GetFileVersion() < (int32)ELuminaEngineVersion::PACKAGE_NAME_TABLE)
        {
            return FArchive::operator<<(Value);
        }

        SerializePackageName(*this, Value, nullptr, Names);
        return *this;
    }
}
