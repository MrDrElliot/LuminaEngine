#include <gtest/gtest.h>

#include "Net/NetPrediction.h"

using namespace Lumina;

namespace
{
    constexpr uint32 Guid = 77;
    constexpr uint32 FirstSeq = 41;

    FNetMoveCommand MakeCommand(uint32 Seq, int8 MoveX, int8 MoveZ, uint16 Yaw, int16 Pitch, uint32 Buttons, bool bJump)
    {
        FNetMoveCommand Command;
        Command.Seq     = Seq;
        Command.MoveX   = MoveX;
        Command.MoveZ   = MoveZ;
        Command.Yaw     = Yaw;
        Command.Pitch   = Pitch;
        Command.Buttons = Buttons;
        Command.bJump   = bJump;
        return Command;
    }

    TVector<uint8> Encode(const TVector<FNetMoveCommand>& Commands)
    {
        TVector<uint8> Buffer;
        NetPrediction::WriteMoveCommands(Buffer, Guid, Commands.GetData(), static_cast<uint32>(Commands.Num()));
        return Buffer;
    }

    TVector<FNetMoveCommand> HeldInput(uint32 Count)
    {
        TVector<FNetMoveCommand> Commands;
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            Commands.Add(MakeCommand(FirstSeq + Index, 127, -64, 16384, -250, 0x5, false));
        }
        return Commands;
    }

    TVector<FNetMoveCommand> ChangingInput(uint32 Count)
    {
        TVector<FNetMoveCommand> Commands;
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            Commands.Add(MakeCommand(FirstSeq + Index, (int8)Index, (int8)-(int32)Index, (uint16)(Index * 1000), (int16)Index, Index, false));
        }
        return Commands;
    }

    uint16 YawDistance(uint16 A, uint16 B)
    {
        const uint16 Forward = (uint16)(A - B);
        return Math::Min(Forward, (uint16)(B - A));
    }
}

TEST(NetMoveCommands, ARunOfHeldAndChangingInputRoundTrips)
{
    const TVector<FNetMoveCommand> Sent =
    {
        MakeCommand(FirstSeq + 0, 10, -20, 100, 5, 0x1, false),
        MakeCommand(FirstSeq + 1, 10, -20, 100, 5, 0x1, true),
        MakeCommand(FirstSeq + 2, -127, 127, 100, 5, 0x1, false),
        MakeCommand(FirstSeq + 3, -127, 127, 65535, -32767, 0x1, false),
        MakeCommand(FirstSeq + 4, -127, 127, 65535, -32767, 0x80000000u, true),
        MakeCommand(FirstSeq + 5, 0, 0, 0, 32767, 0, false),
        MakeCommand(FirstSeq + 6, 0, 0, 0, 32767, 0, false),
    };
    const TVector<uint8> Buffer = Encode(Sent);

    uint32 ReadGuid = 0;
    TVector<FNetMoveCommand> Received;
    ASSERT_TRUE(NetPrediction::ReadMoveCommands(Buffer.GetData(), Buffer.Num(), ReadGuid, Received));
    EXPECT_EQ(ReadGuid, Guid);
    ASSERT_EQ(Received.Num(), Sent.Num());
    for (uint32 Index = 0; Index < (uint32)Sent.Num(); ++Index)
    {
        EXPECT_EQ(Received[Index].Seq, Sent[Index].Seq) << Index;
        EXPECT_EQ(Received[Index].MoveX, Sent[Index].MoveX) << Index;
        EXPECT_EQ(Received[Index].MoveZ, Sent[Index].MoveZ) << Index;
        EXPECT_EQ(Received[Index].Yaw, Sent[Index].Yaw) << Index;
        EXPECT_EQ(Received[Index].Pitch, Sent[Index].Pitch) << Index;
        EXPECT_EQ(Received[Index].Buttons, Sent[Index].Buttons) << Index;
        EXPECT_EQ(Received[Index].bJump, Sent[Index].bJump) << Index;
    }
}

TEST(NetMoveCommands, HeldInputEncodesSmallerThanChangingInput)
{
    constexpr uint32 Count = 16;
    EXPECT_LT(Encode(HeldInput(Count)).Num(), Encode(ChangingInput(Count)).Num());
}

TEST(NetMoveCommands, AnEmptyUploadRoundTrips)
{
    const TVector<uint8> Buffer = Encode({});

    uint32 ReadGuid = 0;
    TVector<FNetMoveCommand> Received;
    ASSERT_TRUE(NetPrediction::ReadMoveCommands(Buffer.GetData(), Buffer.Num(), ReadGuid, Received));
    EXPECT_EQ(ReadGuid, Guid);
    EXPECT_TRUE(Received.IsEmpty());
}

TEST(NetMoveCommands, MoreThanTheUploadLimitIsRefused)
{
    const TVector<uint8> AtLimit = Encode(ChangingInput(NetPrediction::MaxMovesPerUpload));
    const TVector<uint8> OverLimit = Encode(ChangingInput(NetPrediction::MaxMovesPerUpload + 1));

    uint32 ReadGuid = 0;
    TVector<FNetMoveCommand> Received;
    EXPECT_TRUE(NetPrediction::ReadMoveCommands(AtLimit.GetData(), AtLimit.Num(), ReadGuid, Received));
    EXPECT_FALSE(NetPrediction::ReadMoveCommands(OverLimit.GetData(), OverLimit.Num(), ReadGuid, Received));
}

TEST(NetMoveCommands, ATruncatedUploadIsRefused)
{
    const TVector<uint8> Buffer = Encode(ChangingInput(4));

    uint32 ReadGuid = 0;
    TVector<FNetMoveCommand> Received;
    for (SIZE_T Size = 0; Size < (SIZE_T)Buffer.Num(); ++Size)
    {
        EXPECT_FALSE(NetPrediction::ReadMoveCommands(Buffer.GetData(), Size, ReadGuid, Received)) << Size;
    }
}

TEST(NetMoveCommands, AppliedPitchStaysWithinTheReplicatedRange)
{
    EXPECT_FLOAT_EQ(NetPrediction::PitchDegrees(32767), 90.0f);
    EXPECT_FLOAT_EQ(NetPrediction::PitchDegrees(-32767), -90.0f);
    EXPECT_FLOAT_EQ(NetPrediction::PitchDegrees(-250), -2.5f);
}

TEST(NetMoveCommands, YawJustEitherSideOfZeroQuantizesNextToZero)
{
    constexpr uint16 OneDegree = 182;
    const uint16 Zero = NetPrediction::MakeMoveCommand(1, FVector3(0.0f), 0.0f, 0).Yaw;
    const uint16 JustBelow = NetPrediction::MakeMoveCommand(1, FVector3(0.0f), -0.01f, 0).Yaw;
    const uint16 JustUnderATurn = NetPrediction::MakeMoveCommand(1, FVector3(0.0f), 359.99f, 0).Yaw;

    EXPECT_EQ(Zero, 0);
    EXPECT_LT(YawDistance(JustBelow, Zero), OneDegree);
    EXPECT_LT(YawDistance(JustUnderATurn, Zero), OneDegree);
    EXPECT_EQ(JustBelow, JustUnderATurn);
}
