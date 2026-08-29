#include <gtest/gtest.h>

#include "Core/Object/Class.h"
#include "Core/Serialization/NetArchive.h"
#include "Containers/Vector.h"
#include "World/Entity/Components/SimpleAnimationComponent.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaNetReplicatedFieldTests
{
    using Lumina::CStruct;
    using Lumina::FNetArchive;
    using Lumina::SSimpleAnimationComponent;
    using Lumina::TVector;
    using Lumina::uint8;
    using Lumina::uint32;

    CStruct* AnimStruct()
    {
        return SSimpleAnimationComponent::StaticStruct();
    }

    // The wire block a component contributes, which is the changed-field mask followed by those fields.
    TVector<uint8> BuildBlock(const TVector<uint8>& Bytes, const TVector<uint32>& Offsets, const TVector<uint8>& Mask)
    {
        TVector<uint8> Block;
        Block.insert(Block.end(), Mask.begin(), Mask.end());

        const uint32 Count = static_cast<uint32>(Offsets.size()) - 1u;
        for (uint32 i = 0; i < Count; ++i)
        {
            if (Mask[i >> 3] & (1u << (i & 7)))
            {
                Block.insert(Block.end(), Bytes.begin() + Offsets[i], Bytes.begin() + Offsets[i + 1]);
            }
        }
        return Block;
    }

    TVector<uint8> AllSet(uint32 FieldCount)
    {
        TVector<uint8> Mask((FieldCount + 7u) / 8u, 0);
        for (uint32 i = 0; i < FieldCount; ++i)
        {
            Mask[i >> 3] |= static_cast<uint8>(1u << (i & 7));
        }
        return Mask;
    }

    SSimpleAnimationComponent MakeSource()
    {
        SSimpleAnimationComponent Component;
        Component.CurrentTime    = 12.5f;   // not replicated; must not survive the round trip
        Component.PlaybackSpeed  = 2.75f;
        Component.bLooping       = false;
        Component.bPlaying       = false;
        return Component;
    }

    TEST(NetReplicatedField, OffsetsAreByteAlignedAndMonotonic)
    {
        CStruct* Struct = AnimStruct();
        ASSERT_NE(Struct, nullptr);

        SSimpleAnimationComponent Source = MakeSource();

        TVector<uint8>  Bytes;
        TVector<uint32> Offsets;
        FNetArchive     Hooks(Bytes);
        Struct->NetSerializeReplicatedFlat(Hooks, &Source, Bytes, Offsets);

        ASSERT_FALSE(Offsets.empty());
        EXPECT_EQ(Offsets.front(), 0u);
        EXPECT_EQ(Offsets.size() - 1u, Struct->GetNetReplicatedPropertyCount());

        for (size_t i = 1; i < Offsets.size(); ++i)
        {
            EXPECT_GE(Offsets[i], Offsets[i - 1]);
        }

        // The reader byte-aligns after every field, so the writer's boundaries must be the buffer end.
        EXPECT_EQ(Offsets.back(), static_cast<uint32>(Bytes.size()));
    }

    TEST(NetReplicatedField, RoundTripsEveryReplicatedField)
    {
        CStruct* Struct = AnimStruct();
        ASSERT_NE(Struct, nullptr);

        SSimpleAnimationComponent Source = MakeSource();

        TVector<uint8>  Bytes;
        TVector<uint32> Offsets;
        FNetArchive     Hooks(Bytes);
        Struct->NetSerializeReplicatedFlat(Hooks, &Source, Bytes, Offsets);

        const uint32 FieldCount = static_cast<uint32>(Offsets.size()) - 1u;
        const TVector<uint8> Mask  = AllSet(FieldCount);
        const TVector<uint8> Block = BuildBlock(Bytes, Offsets, Mask);

        // The block is mask ++ fields, so the payload starts past the mask.
        const uint32 MaskBytes = (FieldCount + 7u) / 8u;
        ASSERT_GE(Block.size(), MaskBytes);

        SSimpleAnimationComponent Target;
        FNetArchive Reader(Block.data() + MaskBytes, Block.size() - MaskBytes);
        Struct->NetReadReplicatedMasked(Reader, &Target, Mask.data());

        EXPECT_FALSE(Reader.HasError());
        EXPECT_FLOAT_EQ(Target.PlaybackSpeed, Source.PlaybackSpeed);
        EXPECT_EQ(Target.bLooping, Source.bLooping);
        EXPECT_EQ(Target.bPlaying, Source.bPlaying);

        // CurrentTime carries no Replicated flag, so it keeps the target's own value.
        EXPECT_FLOAT_EQ(Target.CurrentTime, SSimpleAnimationComponent{}.CurrentTime);
    }

    TEST(NetReplicatedField, UnchangedFieldsProduceIdenticalBytes)
    {
        CStruct* Struct = AnimStruct();
        ASSERT_NE(Struct, nullptr);

        SSimpleAnimationComponent Source = MakeSource();

        TVector<uint8>  FirstBytes,  SecondBytes;
        TVector<uint32> FirstOffsets, SecondOffsets;

        FNetArchive HooksA(FirstBytes);
        Struct->NetSerializeReplicatedFlat(HooksA, &Source, FirstBytes, FirstOffsets);

        FNetArchive HooksB(SecondBytes);
        Struct->NetSerializeReplicatedFlat(HooksB, &Source, SecondBytes, SecondOffsets);

        // The diff compares field spans byte-for-byte, so an unchanged struct must serialize identically.
        EXPECT_EQ(FirstOffsets, SecondOffsets);
        EXPECT_EQ(FirstBytes, SecondBytes);
    }

    TEST(NetReplicatedField, ChangingOneFieldChangesOnlyThatSpan)
    {
        CStruct* Struct = AnimStruct();
        ASSERT_NE(Struct, nullptr);

        SSimpleAnimationComponent Source = MakeSource();

        TVector<uint8>  BaseBytes;
        TVector<uint32> BaseOffsets;
        FNetArchive     HooksA(BaseBytes);
        Struct->NetSerializeReplicatedFlat(HooksA, &Source, BaseBytes, BaseOffsets);

        Source.PlaybackSpeed += 1.0f;

        TVector<uint8>  NextBytes;
        TVector<uint32> NextOffsets;
        FNetArchive     HooksB(NextBytes);
        Struct->NetSerializeReplicatedFlat(HooksB, &Source, NextBytes, NextOffsets);

        ASSERT_EQ(BaseOffsets, NextOffsets);

        uint32 Differing = 0;
        for (uint32 i = 0; i + 1 < static_cast<uint32>(BaseOffsets.size()); ++i)
        {
            const uint32 Size = BaseOffsets[i + 1] - BaseOffsets[i];
            if (Size != 0 && std::memcmp(BaseBytes.data() + BaseOffsets[i], NextBytes.data() + NextOffsets[i], Size) != 0)
            {
                ++Differing;
            }
        }

        EXPECT_EQ(Differing, 1u);
    }
}
