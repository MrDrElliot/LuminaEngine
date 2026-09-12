#pragma once
#include "Lumina.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Versioning/CoreVersion.h"


namespace Lumina
{
    class FProperty;
}

namespace Lumina
{
    /** Reflected-property serialization tag. */
    struct FPropertyTag
    {
        // the kind on the wire, spelled out as a name only in files older than PACKAGE_NAME_TABLE
        EPropertyTypeFlags Type = EPropertyTypeFlags::None;
        FName Name;
        // bytes of value data following the tag, which is what lets an unknown property be skipped
        int32 Size = 0;

        static_assert((uint32)EPropertyTypeFlags::Count <= 255, "the tag spells the kind in one byte");

        friend FArchive& operator << (FArchive& Ar, FPropertyTag& Data)
        {
            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::PACKAGE_NAME_TABLE)
            {
                uint8 Kind = (uint8)Data.Type;
                Ar << Kind;
                if (Ar.IsReading())
                {
                    Data.Type = (EPropertyTypeFlags)Kind;
                }

                Ar << Data.Name;
                Ar << Data.Size;

                return Ar;
            }

            // the kind used to be a name, and an offset nothing ever read followed the size
            FName TypeName = Ar.IsWriting() ? FName(PropertyTypeToString(Data.Type)) : FName();
            Ar << TypeName;
            Ar << Data.Name;
            Ar << Data.Size;

            int64 UnusedOffset = 0;
            Ar << UnusedOffset;

            if (Ar.IsReading())
            {
                Data.Type = PropertyTypeFromName(TypeName);
            }

            return Ar;
        }
    };
}
