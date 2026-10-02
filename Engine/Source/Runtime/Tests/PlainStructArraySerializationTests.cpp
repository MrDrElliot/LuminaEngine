#include <gtest/gtest.h>

#include "Core/Object/Class.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "World/Entity/Components/FoliageComponent.h"

#include <cstring>

using namespace Lumina;

namespace
{
    // Mirrors the flag ArrayProperty.cpp sets in the element size to mark the schema-plus-raw format.
    constexpr SIZE_T PlainStructArrayFlag = SIZE_T(1) << 63;

    FArrayProperty* InstancesProperty()
    {
        FProperty* Property = SFoliageComponent::StaticStruct()->GetProperty(FName("Instances"));
        return Property != nullptr && Property->GetType() == EPropertyTypeFlags::Vector ? static_cast<FArrayProperty*>(Property) : nullptr;
    }

    SFoliageInstance MakeInstance(uint32 Seed)
    {
        SFoliageInstance Instance;
        Instance.Position  = FVector3((float)Seed, (float)Seed * 2.0f + 0.5f, -(float)Seed);
        Instance.Rotation  = FVector4(0.1f * (float)(Seed % 7), 0.2f, 0.3f, 0.9f);
        Instance.Scale     = FVector3(1.0f + 0.01f * (float)Seed);
        Instance.TypeIndex = (int32)(Seed % 13);
        return Instance;
    }

    void ExpectSame(const SFoliageInstance& A, const SFoliageInstance& B)
    {
        EXPECT_EQ(A.Position.x, B.Position.x);
        EXPECT_EQ(A.Position.y, B.Position.y);
        EXPECT_EQ(A.Position.z, B.Position.z);
        EXPECT_EQ(A.Rotation.x, B.Rotation.x);
        EXPECT_EQ(A.Rotation.w, B.Rotation.w);
        EXPECT_EQ(A.Scale.x, B.Scale.x);
        EXPECT_EQ(A.TypeIndex, B.TypeIndex);
    }

    void WriteField(FArchive& Ar, std::initializer_list<const char*> Path, EPropertyTypeFlags Type, uint32 Offset, uint32 Size)
    {
        uint8 Depth = (uint8)Path.size();
        Ar << Depth;
        for (const char* Segment : Path)
        {
            FName Name(Segment);
            Ar << Name;
        }
        uint8 Kind = (uint8)Type;
        Ar << Kind;
        Ar << Offset;
        Ar << Size;
    }
}

TEST(PlainStructArraySerialization, RoundTripsThroughTheSchemaFormat)
{
    FArrayProperty* Property = InstancesProperty();
    ASSERT_NE(Property, nullptr);

    constexpr uint32 Count = 1000;
    TVector<SFoliageInstance> Source;
    for (uint32 i = 0; i < Count; ++i)
    {
        Source.push_back(MakeInstance(i));
    }

    TVector<uint8> Bytes;
    FMemoryWriter Writer(Bytes);
    Property->Serialize(Writer, &Source);

    // A tag per field per element would be several times this, so staying under it proves the schema path ran.
    EXPECT_LT(Bytes.size(), (size_t)Count * sizeof(SFoliageInstance) + 4096);

    TVector<SFoliageInstance> Loaded;
    FMemoryReader Reader(Bytes);
    Property->Serialize(Reader, &Loaded);
    ASSERT_FALSE(Reader.HasError());
    ASSERT_EQ(Loaded.size(), (size_t)Count);
    for (uint32 i = 0; i < Count; ++i)
    {
        ExpectSame(Loaded[i], Source[i]);
    }
}

TEST(PlainStructArraySerialization, StillReadsTheTaggedFormat)
{
    FArrayProperty* Property = InstancesProperty();
    ASSERT_NE(Property, nullptr);

    TVector<SFoliageInstance> Source = { MakeInstance(3), MakeInstance(8) };

    TVector<uint8> Bytes;
    FMemoryWriter Writer(Bytes);
    SIZE_T Count = Source.size();
    SIZE_T ElementSize = Property->GetInternalProperty()->GetElementSize();
    Writer << Count;
    Writer << ElementSize;
    for (SFoliageInstance& Instance : Source)
    {
        Property->GetInternalProperty()->Serialize(Writer, &Instance);
    }

    TVector<SFoliageInstance> Loaded;
    FMemoryReader Reader(Bytes);
    Property->Serialize(Reader, &Loaded);
    ASSERT_FALSE(Reader.HasError());
    ASSERT_EQ(Loaded.size(), Source.size());
    ExpectSame(Loaded[0], Source[0]);
    ExpectSame(Loaded[1], Source[1]);
}

TEST(PlainStructArraySerialization, RemapsAnOlderLayoutByFieldName)
{
    FArrayProperty* Property = InstancesProperty();
    ASSERT_NE(Property, nullptr);

    // An older element with a narrower TypeIndex, Position.x stored as a double, and a field the struct has since dropped.
    struct FOldElement
    {
        int16  TypeIndex;
        uint8  Padding[6];
        double PositionX;
        float  Removed;
        float  Tail;
    };
    static_assert(sizeof(FOldElement) == 24);

    TVector<uint8> Bytes;
    FMemoryWriter Writer(Bytes);
    SIZE_T Count = 2;
    SIZE_T FlaggedStride = sizeof(FOldElement) | PlainStructArrayFlag;
    uint32 NumFields = 3;
    Writer << Count;
    Writer << FlaggedStride;
    Writer << NumFields;
    WriteField(Writer, { "TypeIndex" }, EPropertyTypeFlags::Int16, 0, 2);
    WriteField(Writer, { "Position", "x" }, EPropertyTypeFlags::Double, 8, 8);
    WriteField(Writer, { "Removed" }, EPropertyTypeFlags::Float, 16, 4);

    FOldElement Old[2] = {};
    Old[0].TypeIndex = 5;
    Old[0].PositionX = 12.5;
    Old[1].TypeIndex = -2;
    Old[1].PositionX = -3.25;
    Writer.Serialize(Old, sizeof(Old));

    TVector<SFoliageInstance> Loaded;
    FMemoryReader Reader(Bytes);
    Property->Serialize(Reader, &Loaded);
    ASSERT_FALSE(Reader.HasError());
    ASSERT_EQ(Loaded.size(), 2u);

    EXPECT_EQ(Loaded[0].TypeIndex, 5);
    EXPECT_EQ(Loaded[1].TypeIndex, -2);
    EXPECT_FLOAT_EQ(Loaded[0].Position.x, 12.5f);
    EXPECT_FLOAT_EQ(Loaded[1].Position.x, -3.25f);

    // Fields the old layout never had keep the struct's defaults.
    const SFoliageInstance Defaults;
    EXPECT_EQ(Loaded[0].Position.y, Defaults.Position.y);
    EXPECT_EQ(Loaded[0].Rotation.w, Defaults.Rotation.w);
    EXPECT_EQ(Loaded[0].Scale.x, Defaults.Scale.x);
}
