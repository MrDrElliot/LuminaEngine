#include <gtest/gtest.h>
#include "Core/Reflection/PropertyTypeTraits.h"

#include "Core/Reflection/Type/LuminaTypes.h"

using namespace Lumina;

namespace
{
    struct FClamped
    {
        float Value = 0.0f;
    };

    constexpr FMetadataEntry GClampedMetadata[] =
    {
        { "ClampMin", "0.5f" },
        { "ClampMax", "+12" },
        { "Delta", "fast" },
        { "Units", "" },
    };

    constexpr FPropertyDesc GClampedDesc
    {
        .Name = "Value",
        .Offset = offsetof(FClamped, Value),
        .Type = PropertyTypeOf<float>,
        .Metadata = GClampedMetadata,
        .NumMetadata = (uint16)std::size(GClampedMetadata),
        .EditorFlags = EPropertyEditorFlags::NoDrag | EPropertyEditorFlags::Color,
    };
}

TEST(PropertyMetadata, ANumberReadsTheWayItIsWrittenInCpp)
{
    const FFloatProperty Property(GClampedDesc);

    EXPECT_EQ(Property.GetMetadataNumber("ClampMin"), TOptional<double>(0.5));
    EXPECT_EQ(Property.GetMetadataNumber<int32>("ClampMax"), TOptional<int32>(12));
}

TEST(PropertyMetadata, AnythingButANumberReadsAsUnset)
{
    const FFloatProperty Property(GClampedDesc);

    EXPECT_FALSE(Property.GetMetadataNumber("Delta").IsSet());
    EXPECT_FALSE(Property.GetMetadataNumber("Units").IsSet());
    EXPECT_FALSE(Property.GetMetadataNumber("Missing").IsSet());
}

TEST(PropertyMetadata, EditorFlagsAreBitsRatherThanEntries)
{
    const FFloatProperty Property(GClampedDesc);

    EXPECT_TRUE(Property.HasEditorFlag(EPropertyEditorFlags::NoDrag));
    EXPECT_TRUE(Property.HasEditorFlag(EPropertyEditorFlags::Color));
    EXPECT_FALSE(Property.HasEditorFlag(EPropertyEditorFlags::Multiline));
    EXPECT_FALSE(Property.HasMetadata("NoDrag"));
}

TEST(PropertyMetadata, EveryEditorFlagNameMatchesItsBit)
{
    for (uint16 Bit = 0; Bit < PropertyEditorFlagBit::Count; ++Bit)
    {
        EXPECT_NE(PropertyEditorFlagNames[Bit], nullptr);
    }
    EXPECT_STREQ(PropertyEditorFlagNames[PropertyEditorFlagBit::NoDrag], "NoDrag");
    EXPECT_EQ((uint16)EPropertyEditorFlags::NoDrag, 1u << PropertyEditorFlagBit::NoDrag);
}
