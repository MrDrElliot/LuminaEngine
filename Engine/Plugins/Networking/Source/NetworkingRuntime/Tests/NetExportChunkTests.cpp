#include <gtest/gtest.h>

#include "Containers/StringFormat.h"
#include "Core/Serialization/NetArchive.h"
#include "Net/NetReplication.h"
#include "Net/NetWorldState.h"

using namespace Lumina;

namespace
{
    constexpr uint32 NumObjects   = 10000;
    constexpr uint32 NumNames     = 5000;
    constexpr uint32 MissingIndex = NumObjects + NumNames + 1;

    // Splits Batch into its messages, checking each fits the frame and returns the sum of their counts.
    uint32 ForEachExport(const TVector<uint8>& Batch, const TFunction<void(const uint8*, SIZE_T)>& Apply)
    {
        uint32 TotalCount = 0;
        uint32 NumMessages = 0;
        Net::ForEachFramedMessage(Batch.GetData(), static_cast<SIZE_T>(Batch.Num()), [&](const uint8* Msg, SIZE_T Size)
        {
            EXPECT_LE(Size, Net::MaxFramedMessageSize);
            FNetArchive Reader(Msg, Size);
            uint8  Type  = 0;
            uint16 Count = 0;
            Reader << Type;
            Reader << Count;
            TotalCount += Count;
            ++NumMessages;
            Apply(Msg, Size);
        });
        EXPECT_GT(NumMessages, 1u);
        return TotalCount;
    }
}

TEST(NetExportChunk, LargeObjectExportSplitsAndRecoversEveryIndex)
{
    FNetObjectMap Out;
    TVector<uint32> Indices;
    for (uint32 Index = 1; Index <= NumObjects; ++Index)
    {
        Out.IndexToGuid[Index] = FGuid::New();
        Indices.Add(Index);
    }
    Indices.Add(MissingIndex);

    TVector<uint8> Batch;
    Net::BuildObjectExport(Out, Indices, Batch);

    FNetObjectMap In;
    const uint32 TotalCount = ForEachExport(Batch, [&](const uint8* Msg, SIZE_T Size) { Net::ApplyObjectExport(In, Msg, Size); });

    EXPECT_EQ(TotalCount, NumObjects);
    ASSERT_EQ(In.IndexToGuid.Num(), Out.IndexToGuid.Num());
    for (const auto& [Index, Guid] : Out.IndexToGuid)
    {
        const FGuid* Received = In.IndexToGuid.Find(Index);
        ASSERT_NE(Received, nullptr);
        EXPECT_EQ(*Received, Guid);
    }
}

TEST(NetExportChunk, ObjectExportPastTheIndexCapIsRefused)
{
    constexpr uint32 NumSent = Net::MaxIncomingNetIndex + 1000;
    FNetObjectMap Out;
    TVector<uint32> Indices;
    for (uint32 Index = 0; Index < NumSent; ++Index)
    {
        Out.IndexToGuid[Index] = FGuid::New();
        Indices.Add(Index);
    }

    TVector<uint8> Batch;
    Net::BuildObjectExport(Out, Indices, Batch);

    FNetObjectMap In;
    ForEachExport(Batch, [&](const uint8* Msg, SIZE_T Size) { Net::ApplyObjectExport(In, Msg, Size); });

    EXPECT_EQ(static_cast<uint32>(In.IndexToGuid.Num()), Net::MaxIncomingNetIndex);
    EXPECT_EQ(In.IndexToGuid.Find(Net::MaxIncomingNetIndex), nullptr);
    EXPECT_TRUE(In.bLoggedRefusal);
}

TEST(NetExportChunk, ObjectExportSkipsARefusedIndexInsteadOfDroppingLaterEntries)
{
    FNetObjectMap Out;
    const uint32 BadIndex = Net::MaxIncomingNetIndex + 5;
    const FGuid FirstGuid  = FGuid::New();
    const FGuid BadGuid    = FGuid::New();
    const FGuid SecondGuid = FGuid::New();
    Out.IndexToGuid[1] = FirstGuid;
    Out.IndexToGuid[BadIndex] = BadGuid;
    Out.IndexToGuid[2] = SecondGuid;

    TVector<uint8> Batch;
    Net::BuildObjectExport(Out, { 1, BadIndex, 2 }, Batch);

    FNetObjectMap In;
    uint32 NumMessages = 0;
    Net::ForEachFramedMessage(Batch.GetData(), static_cast<SIZE_T>(Batch.Num()), [&](const uint8* Msg, SIZE_T Size)
    {
        ++NumMessages;
        Net::ApplyObjectExport(In, Msg, Size);
    });

    EXPECT_EQ(NumMessages, 1u);
    EXPECT_EQ(In.IndexToGuid.Num(), 2);
    const FGuid* First = In.IndexToGuid.Find(1);
    ASSERT_NE(First, nullptr);
    EXPECT_EQ(*First, FirstGuid);
    EXPECT_EQ(In.IndexToGuid.Find(BadIndex), nullptr);
    const FGuid* Second = In.IndexToGuid.Find(2);
    ASSERT_NE(Second, nullptr);
    EXPECT_EQ(*Second, SecondGuid);
}

TEST(NetExportChunk, OutgoingNameIndexExhaustionStopsMintingAtTheCap)
{
    FNetWorldState State;
    TVector<uint8> Bytes;
    FNetArchive Ar(Bytes);
    Net::BindWriters(Ar, State);

    for (uint32 Index = 0; Index < Net::MaxIncomingNetIndex; ++Index)
    {
        Ar.NameToNetIndex(FName(Format("Exhaust{}", Index)));
    }

    EXPECT_TRUE(State.OutNames.bLoggedRefusal);
    EXPECT_EQ(State.OutNames.NextIndex, Net::MaxIncomingNetIndex);

    const int32 NamesBeforeRetry = State.OutNames.IndexToName.Num();
    const uint32 RefusedIndex = Ar.NameToNetIndex(FName("OneMoreName"));
    EXPECT_EQ(RefusedIndex, 0u);
    EXPECT_EQ(State.OutNames.NextIndex, Net::MaxIncomingNetIndex);
    EXPECT_EQ(State.OutNames.IndexToName.Num(), NamesBeforeRetry);
}

TEST(NetExportChunk, LargeNameExportSplitsAndRecoversEveryIndex)
{
    FNetNameMap Out;
    TVector<uint32> Indices;
    const FString Padding(200, 'n');
    for (uint32 Index = 1; Index <= NumNames; ++Index)
    {
        Out.IndexToName[Index] = FName(Padding + Format("{}", Index));
        Indices.Add(Index);
    }
    Indices.Add(MissingIndex);

    TVector<uint8> Batch;
    Net::BuildNameExport(Out, Indices, Batch);

    FNetNameMap In;
    const uint32 TotalCount = ForEachExport(Batch, [&](const uint8* Msg, SIZE_T Size) { Net::ApplyNameExport(In, Msg, Size); });

    EXPECT_EQ(TotalCount, NumNames);
    ASSERT_EQ(In.IndexToName.Num(), Out.IndexToName.Num());
    for (const auto& [Index, Name] : Out.IndexToName)
    {
        const FName* Received = In.IndexToName.Find(Index);
        ASSERT_NE(Received, nullptr);
        EXPECT_EQ(*Received, Name);
    }
}
