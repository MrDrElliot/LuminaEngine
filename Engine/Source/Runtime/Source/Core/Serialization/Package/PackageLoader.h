#pragma once
#include "Core/Object/Object.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Memory/Memory.h"
#include "Memory/SmartPtr.h"
#include "Core/Templates/LuminaTemplate.h"

namespace Lumina
{
    class CPackage;
    
    // A package's file bytes, read only once loaded and shared by every reader over them. Held by
    // shared pointer so a reader in flight keeps the buffer alive after the package drops its cache.
    struct FPackageFileBytes
    {
        FPackageFileBytes(void* InData, int64 InSize) : Data(InData), Size(InSize) {}

        ~FPackageFileBytes()
        {
            if (Data != nullptr)
            {
                Memory::Free(Data);
            }
        }

        FPackageFileBytes(const FPackageFileBytes&) = delete;
        FPackageFileBytes& operator=(const FPackageFileBytes&) = delete;

        void* Data = nullptr;
        int64 Size = 0;
    };

    // One cursor per reader, so two threads loading different exports never seek each other's reads.
    class FPackageLoader : public FBufferReader
    {
    public:

        using FArchive::operator<<;

        FPackageLoader(TSharedPtr<FPackageFileBytes> InBytes, CPackage* InPackage)
            : FBufferReader(InBytes->Data, InBytes->Size, false)
            , Bytes(Move(InBytes))
            , Package(InPackage)
        {
        }

        virtual FArchive& operator<<(CObject*& Value) override;
        virtual FArchive& operator<<(FObjectHandle& Value) override;

    private:

        // Owns the read, not the bytes; the shared reference is what keeps them alive.
        TSharedPtr<FPackageFileBytes> Bytes;
        CPackage* Package;
    };
}
