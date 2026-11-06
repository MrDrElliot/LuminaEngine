#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Containers/HashTable.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Serialization/MemoryArchiver.h"

namespace Lumina
{
    constexpr uint32 kPackageNameNumberBit = 0x80000000u;
    constexpr uint32 kPackageNameSlotMask  = 0x7FFFFFFFu;

    // each distinct name once per package, so one on every property header interns once, not per occurrence
    class RUNTIME_API FPackageNameMap
    {
    public:

        // slot for this name's base text, assigned in first-seen order, so saves stay reproducible
        uint32 Slot(const FName& Name);

        // written last, because no name is known until everything that could carry one has been serialized
        void Serialize(FArchive& Ar) const;

        int32 Num() const { return Slots.Num(); }

        const TVector<FName>& GetSlots() const { return Slots; }

    private:

        THashMap<uint32, uint32> BaseToSlot;
        TVector<FName>           Slots;
    };

    class RUNTIME_API FPackageNameTable
    {
    public:

        FPackageNameTable() = default;
        explicit FPackageNameTable(TVector<FName> InSlots) : Slots(Move(InSlots)) {}

        void Serialize(FArchive& Ar);

        // NAME_None for a slot outside the table, so a reader that skipped loading it fails visibly
        FName Resolve(uint32 Slot, bool bHasNumber, uint32 ExternalNumber) const;

        int32 Num() const { return Slots.Num(); }

    private:

        TVector<FName> Slots;
    };

    // a slot with the high bit flagging a number that follows, so a numbered name shares its base entry
    RUNTIME_API void SerializePackageName(FArchive& Ar, FName& Value, FPackageNameMap* Map, const FPackageNameTable* Table);

    // reads a decompressed container with no CPackage behind it, for the registry scan and for delete
    class RUNTIME_API FPackageContainerReader : public FMemoryReader
    {
    public:

        explicit FPackageContainerReader(const TVector<uint8>& InBytes) : FMemoryReader(InBytes) {}

        using FArchive::operator<<;

        FArchive& operator<<(FName& Value) override;

        void SetNameTable(const FPackageNameTable* InNames) { Names = InNames; }

    private:

        const FPackageNameTable* Names = nullptr;
    };

    // the writing side of FPackageContainerReader, numbering names in a map of its own
    class RUNTIME_API FPackageContainerWriter : public FMemoryWriter
    {
    public:

        explicit FPackageContainerWriter(TVector<uint8>& InBytes) : FMemoryWriter(InBytes) {}

        using FArchive::operator<<;

        FArchive& operator<<(FName& Value) override;

        const FPackageNameMap& GetNameMap() const { return NameMap; }

    private:

        FPackageNameMap NameMap;
    };
}
