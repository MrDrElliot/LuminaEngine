#pragma once
#include "Lumina.h"
#include "Containers/Name.h"


namespace Lumina
{
    class FProperty;
}

namespace Lumina
{
    /** Reflected-property serialization tag. */
    struct FPropertyTag
    {
        FName Type;
        FName Name;
        int32 Size = 0;
        /** Offset into the buffer in bytes. */
        int64 Offset = 0;
        
        
        friend FArchive& operator << (FArchive& Ar, FPropertyTag& Data)
        {
            Ar << Data.Type;
            Ar << Data.Name;
            Ar << Data.Size;
            Ar << Data.Offset;

            return Ar;
        }
    };
}
