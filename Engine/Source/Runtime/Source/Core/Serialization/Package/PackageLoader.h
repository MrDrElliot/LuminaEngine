#pragma once
#include "Containers/HashTable.h"
#include "Core/Object/Object.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Memory/Memory.h"
#include "Memory/SmartPtr.h"
#include "Core/Templates/LuminaTemplate.h"
#include "PackageNameTable.h"

namespace Lumina
{
    class CPackage;

    // One load and every reference it reaches through the archive; a graph load names the packages whose PostLoad it runs itself.
    struct FLoadRequest
    {
        const THashSet<CPackage*>* DeferredPostLoad = nullptr;

        NODISCARD bool DefersPostLoad(const CObject* Object) const
        {
            return DeferredPostLoad != nullptr && Object != nullptr && DeferredPostLoad->Contains(Object->GetPackage());
        }
    };
    
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
        FPackageFileBytes(FPackageFileBytes&&) = delete;
        FPackageFileBytes& operator=(FPackageFileBytes&&) = delete;

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

        // names come back as slots in the package's table, older files carry text
        virtual FArchive& operator<<(FName& Value) override;

        // held, not borrowed, so a concurrent save cannot replace it under a read in flight
        void SetNameTable(TSharedPtr<const FPackageNameTable> InNames) { Names = Move(InNames); }

        // References read through this archive join Request, so they share its PostLoad deferral.
        void SetLoadRequest(FLoadRequest* InRequest) { Request = InRequest; }

        // Every four-byte window is a candidate, so the tables come out a superset of what the bytes refer to.
        bool DescribeCutBytes(FPortableBytes& Cut) const override;

        // Mirrors FPackageSaver, since FTextureSourceFile picks its inline or bulk layout from this answer on read too.
        bool SupportsBulkData() const override { return true; }

    private:

        // Owns the read, not the bytes; the shared reference is what keeps them alive.
        TSharedPtr<FPackageFileBytes> Bytes;
        CPackage* Package;
        FLoadRequest* Request = nullptr;
        TSharedPtr<const FPackageNameTable> Names;
    };
}
