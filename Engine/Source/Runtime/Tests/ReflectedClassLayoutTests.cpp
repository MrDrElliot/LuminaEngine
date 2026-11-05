#include <gtest/gtest.h>

#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"

using namespace Lumina;

namespace
{
    bool Contains(TSpan<FProperty* const> Haystack, const FProperty* Needle)
    {
        for (const FProperty* Candidate : Haystack)
        {
            if (Candidate == Needle)
            {
                return true;
            }
        }
        return false;
    }

    TVector<CStruct*> AllReflectedStructs()
    {
        RegisterPendingTypes();

        TVector<CStruct*> Structs;
        ForEachObject([&Structs](CObjectBase* Object, int32)
        {
            if (Object != nullptr && Object->IsA<CStruct>())
            {
                Structs.Add(static_cast<CStruct*>(Object));
            }
        });
        return Structs;
    }
}

// Link caches the flattened list and latches. A class published before its properties were attached used to
// cache an empty one permanently, which read as a type with no properties to serialization and the editor.
TEST(ReflectedClassLayout, EveryOwnPropertyReachesTheFlattenedList)
{
    for (CStruct* Struct : AllReflectedStructs())
    {
        const TSpan<FProperty* const> Own = Struct->GetOwnProperties();
        if (Own.IsEmpty())
        {
            continue;
        }

        // Only a linked type has a flattened list to check; Link is what builds it.
        Struct->Link();

        const TSpan<FProperty* const> All = Struct->GetProperties();
        for (FProperty* Property : Own)
        {
            EXPECT_TRUE(Contains(All, Property))
                << *Struct->GetName().ToString() << " declares '"
                << *Property->GetPropertyName().ToString() << "' but its flattened list does not carry it";
        }
    }
}

// A derived type's flattened list is its own properties plus every one its bases declare.
TEST(ReflectedClassLayout, TheFlattenedListCarriesEverySuperProperty)
{
    for (CStruct* Struct : AllReflectedStructs())
    {
        CStruct* Super = Struct->GetSuperStruct();
        if (Super == nullptr || Super->GetOwnProperties().IsEmpty())
        {
            continue;
        }

        Struct->Link();

        const TSpan<FProperty* const> All = Struct->GetProperties();
        for (FProperty* Property : Super->GetOwnProperties())
        {
            EXPECT_TRUE(Contains(All, Property))
                << *Struct->GetName().ToString() << " does not carry '"
                << *Property->GetPropertyName().ToString() << "' from its base "
                << *Super->GetName().ToString();
        }
    }
}
