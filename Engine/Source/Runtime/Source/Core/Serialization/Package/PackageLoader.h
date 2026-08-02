#pragma once
#include "Core/Object/Object.h"
#include "Core/Serialization/MemoryArchiver.h"

namespace Lumina
{
    class CPackage;
    
    class FPackageLoader : public FBufferReader
    {
    public:

        using FArchive::operator<<;

        explicit FPackageLoader(void* Data, int64 Size, CPackage* InPackage)
            : FBufferReader(Data, Size, true)
            , Package(InPackage)
        {
        }
        
        virtual FArchive& operator<<(CObject*& Value) override;
        virtual FArchive& operator<<(FObjectHandle& Value) override;


    private:

        CPackage* Package;
    };
}
