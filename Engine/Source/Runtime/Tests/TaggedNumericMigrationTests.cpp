#include <gtest/gtest.h>

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Serialization/MemoryArchiver.h"

using namespace Lumina;

namespace
{
    struct FStoredInt32
    {
        int32 Value = 0;
    };

    struct FLoadedUInt8
    {
        uint8 Value = 0;
        uint8 SetterCalls = 0;
        uint8 Padding[2] = {};
        uint32 Adjacent = 0;
    };

    struct FLoadedFloat
    {
        float Value = 0.0f;
        uint32 Adjacent = 0;
    };

    void SetLoadedUInt8(void* Container, const void* Value)
    {
        FLoadedUInt8& Loaded = *static_cast<FLoadedUInt8*>(Container);
        Loaded.Value = *static_cast<const uint8*>(Value);
        ++Loaded.SetterCalls;
    }

    CStruct* MakeNumericStruct(const char* Name, uint32 Size, FProperty* const* Properties, uint32 Count)
    {
        CStruct* Struct = NewObject<CStruct>(nullptr, Name);
        Struct->AddToRoot();
        Struct->Size = Size;
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            Struct->AddProperty(Properties[Index]);
        }
        Struct->Link();
        return Struct;
    }

    CStruct* StoredInt32Struct()
    {
        static const FNumericPropertyParams ValueParams = {
            "Value", EPropertyFlags::None, EPropertyTypeFlags::Int32, nullptr, nullptr, offsetof(FStoredInt32, Value)
        };
        static FInt32Property ValueProperty(&ValueParams);
        static FProperty* const Properties[] = { &ValueProperty };
        static CStruct* Struct = MakeNumericStruct("FTaggedMigrationStoredInt32", sizeof(FStoredInt32), Properties, 1);
        return Struct;
    }

    CStruct* LoadedUInt8Struct()
    {
        static const FNumericPropertyParams ValueParams = {
            "Value", EPropertyFlags::None, EPropertyTypeFlags::UInt8, &SetLoadedUInt8, nullptr, offsetof(FLoadedUInt8, Value)
        };
        static const FNumericPropertyParams AdjacentParams = {
            "Adjacent", EPropertyFlags::None, EPropertyTypeFlags::UInt32, nullptr, nullptr, offsetof(FLoadedUInt8, Adjacent)
        };
        static TPropertyWithSetterAndGetter<FUInt8Property> ValueProperty(&ValueParams);
        static FUInt32Property AdjacentProperty(&AdjacentParams);
        static FProperty* const Properties[] = { &ValueProperty, &AdjacentProperty };
        static CStruct* Struct = MakeNumericStruct("FTaggedMigrationLoadedUInt8", sizeof(FLoadedUInt8), Properties, 2);
        return Struct;
    }

    CStruct* LoadedFloatStruct()
    {
        static const FNumericPropertyParams ValueParams = {
            "Value", EPropertyFlags::None, EPropertyTypeFlags::Float, nullptr, nullptr, offsetof(FLoadedFloat, Value)
        };
        static const FNumericPropertyParams AdjacentParams = {
            "Adjacent", EPropertyFlags::None, EPropertyTypeFlags::UInt32, nullptr, nullptr, offsetof(FLoadedFloat, Adjacent)
        };
        static FFloatProperty ValueProperty(&ValueParams);
        static FUInt32Property AdjacentProperty(&AdjacentParams);
        static FProperty* const Properties[] = { &ValueProperty, &AdjacentProperty };
        static CStruct* Struct = MakeNumericStruct("FTaggedMigrationLoadedFloat", sizeof(FLoadedFloat), Properties, 2);
        return Struct;
    }

    TVector<uint8> SaveInt32(int32 Value)
    {
        FStoredInt32 Source{ Value };
        TVector<uint8> Bytes;
        FMemoryWriter Writer(Bytes);
        StoredInt32Struct()->SerializeTaggedProperties(Writer, &Source);
        return Bytes;
    }
}

TEST(TaggedNumericMigration, Int32ToUInt8LeavesAdjacentFieldUntouched)
{
    const TVector<uint8> Bytes = SaveInt32(37);
    FLoadedUInt8 Loaded{};
    Loaded.Adjacent = 0x78563412u;

    FMemoryReader Reader(Bytes);
    LoadedUInt8Struct()->SerializeTaggedProperties(Reader, &Loaded);

    EXPECT_FALSE(Reader.HasError());
    EXPECT_EQ(Loaded.Value, 37u);
    EXPECT_EQ(Loaded.SetterCalls, 1u);
    EXPECT_EQ(Loaded.Adjacent, 0x78563412u);
}

TEST(TaggedNumericMigration, OutOfRangeValueLeavesDestinationUntouched)
{
    const TVector<uint8> Bytes = SaveInt32(300);
    FLoadedUInt8 Loaded{};
    Loaded.Value = 5;
    Loaded.Adjacent = 0x78563412u;

    FMemoryReader Reader(Bytes);
    LoadedUInt8Struct()->SerializeTaggedProperties(Reader, &Loaded);

    EXPECT_FALSE(Reader.HasError());
    EXPECT_EQ(Loaded.Value, 5u);
    EXPECT_EQ(Loaded.SetterCalls, 0u);
    EXPECT_EQ(Loaded.Adjacent, 0x78563412u);
}

TEST(TaggedNumericMigration, Int32ToFloatLeavesAdjacentFieldUntouched)
{
    const TVector<uint8> Bytes = SaveInt32(37);
    FLoadedFloat Loaded{ 0.0f, 0x78563412u };

    FMemoryReader Reader(Bytes);
    LoadedFloatStruct()->SerializeTaggedProperties(Reader, &Loaded);

    EXPECT_FALSE(Reader.HasError());
    EXPECT_FLOAT_EQ(Loaded.Value, 37.0f);
    EXPECT_EQ(Loaded.Adjacent, 0x78563412u);
}

TEST(TaggedNumericMigration, RoundedInt64LimitsAreRejected)
{
    EXPECT_FALSE(IsValueValidForType(9223372036854775808.0, EPropertyTypeFlags::Int64));
    EXPECT_FALSE(IsValueValidForType(18446744073709551616.0, EPropertyTypeFlags::UInt64));
}
