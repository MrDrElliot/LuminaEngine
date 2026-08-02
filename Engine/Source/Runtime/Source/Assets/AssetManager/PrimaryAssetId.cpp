#include "RuntimePCH.h"
#include "PrimaryAssetId.h"

#include "Core/Serialization/Archiver.h"


namespace Lumina
{
    FArchive& operator<<(FArchive& Ar, FPrimaryAssetId& Self)
    {
        Ar << Self.Name;
        return Ar;
    }
}
