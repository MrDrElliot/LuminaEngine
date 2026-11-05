#include "RuntimePCH.h"
#include "PackageNameTable.h"

#include "Core/Assertions/Assert.h"
#include "Core/Versioning/CoreVersion.h"
#include "Log/Log.h"

namespace Lumina
{

    uint32 FPackageNameMap::Slot(const FName& Name)
    {
        const FName Base = Name.GetBaseName();
        const uint32 Key = Base.GetID();

        if (const auto It = BaseToSlot.Find(Key); It != nullptr)
        {
            return (*It);
        }

        const uint32 NewSlot = (uint32)Slots.Num();
        DEBUG_ASSERT(NewSlot < kPackageNameNumberBit);
        BaseToSlot.TryAdd(Key, NewSlot);
        Slots.Add(Base);
        return NewSlot;
    }

    void FPackageNameMap::Serialize(FArchive& Ar) const
    {
        uint32 Count = (uint32)Slots.Num();
        Ar << Count;

        // length then bytes, the encoding an FString writes, without building one per name
        for (const FName& Name : Slots)
        {
            // none renders as the text "NAME_None", so it goes on the wire empty, as FArchive spells it
            const bool  bNone  = Name.IsNone();
            const char* Text   = bNone ? "" : *Name;
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
        Slots.Reset();

        uint32 Count = 0;
        Ar << Count;

        if (Count > Ar.GetMaxSerializeSize() || !Ar.CanHoldCount(Count, 64))
        {
            LOG_ERROR("Package name table claims {} entries (max {}); the package is corrupt", Count, Ar.GetMaxSerializeSize());
            Ar.SetHasError(true);
            return;
        }

        Slots.Reserve(Count);

        // straight into the stack and interned once, an FString here cost three allocator calls per name
        char Stack[512];

        for (uint32 Index = 0; Index < Count; ++Index)
        {
            uint64 Length = 0;
            Ar << Length;

            if (Length > Ar.GetMaxSerializeSize() || !Ar.CanHoldCount(Length, 8))
            {
                LOG_ERROR("Package name {} claims {} bytes; the name table is corrupt", Index, Length);
                Ar.SetHasError(true);
                Slots.Reset();
                return;
            }

            if (Length == 0)
            {
                Slots.Add(NAME_None);
                continue;
            }

            // Interned unsplit since save already split the base, and only after the length check so a corrupt file adds nothing.
            if (Length < sizeof(Stack))
            {
                Ar.Serialize(Stack, (int64)Length);
                if (Ar.HasError())
                {
                    Slots.Reset();
                    return;
                }

                Stack[Length] = '\0';
                Slots.Add(FName(Stack, 0).GetBaseName());
            }
            else
            {
                FString Heap;
                Heap.SetLen(IntCastChecked<int32>(Length));
                Ar.Serialize(Heap.GetData(), (int64)Length);
                if (Ar.HasError())
                {
                    Slots.Reset();
                    return;
                }

                Slots.Add(FName(*Heap, 0).GetBaseName());
            }
        }
    }

    FName FPackageNameTable::Resolve(uint32 Slot, bool bHasNumber, uint32 ExternalNumber) const
    {
        if (Slot >= static_cast<uint32>(Slots.Num()))
        {
            LOG_ERROR("Package name slot {} is outside the {}-entry name table", Slot, Slots.Num());
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
                Packed |= kPackageNameNumberBit;
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

        const bool bHasNumber = (Packed & kPackageNameNumberBit) != 0;
        uint32 Number = 0;
        if (bHasNumber)
        {
            Ar << Number;
        }

        // One bad slot means the stream is misaligned, so the archive fails rather than logging every name after it.
        if ((Packed & kPackageNameSlotMask) >= static_cast<uint32>(Table->Num()))
        {
            if (!Ar.HasError())
            {
                LOG_ERROR("Package name slot {} is outside the {}-entry name table; failing the archive", Packed & kPackageNameSlotMask, Table->Num());
            }
            Ar.SetHasError(true);
            Value = NAME_None;
            return;
        }

        Value = Table->Resolve(Packed & kPackageNameSlotMask, bHasNumber, Number);
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

    FArchive& FPackageContainerWriter::operator<<(FName& Value)
    {
        if (GetFileVersion() < (int32)ELuminaEngineVersion::PACKAGE_NAME_TABLE)
        {
            return FArchive::operator<<(Value);
        }

        SerializePackageName(*this, Value, &NameMap, nullptr);
        return *this;
    }
}
