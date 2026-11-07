#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "World/Entity/Components/TransformComponent.h"

#include "Components/RepTransformComponent.h"
#include "Core/Serialization/NetArchive.h"
#include "Net/NetWorldState.h"
#include "Systems/NetworkSystem.h"

using namespace Lumina;
using namespace Lumina::NetworkSystemInternal;

namespace
{
    constexpr uint32 Guid = NetGUID_DynamicStart + 1;

    FQuat RoundTrip(ENetLODTier Tier, const FQuat& Rot)
    {
        ECS::FRegistry Registry;
        FNetWorldState& State = Registry.Ctx().Emplace<FNetWorldState>();
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<STransformComponent>(Entity);
        State.GuidTable.Register(SNetGUID{ Guid }, Entity);

        FTransformSendRecord Record;
        Record.Guid = Guid;
        Record.Tier = Tier;
        Record.Pos  = FVector3(3.0f, 1.0f, -2.0f);
        Record.Rot  = Rot;

        TVector<uint8> Buffer;
        FNetArchive Writer(Buffer);
        WriteTransformRecord(Writer, Record, FVector3(0.0f));

        FNetArchive Reader(Buffer.GetData(), static_cast<SIZE_T>(Buffer.Num()));
        EXPECT_TRUE(ReadTransformIntoRing(Registry, State, Reader, 1.0, false, 0, FVector3(0.0f)));
        const FRepTransform* Rep = Registry.TryGet<ECS::Read<FRepTransform>>(Entity);
        return Rep != nullptr && Rep->Ring.Count == 1 ? Rep->Ring.Newest().Rot : FQuat();
    }

    FQuat YawQuat(float Degrees)
    {
        return Math::FromAxisAngle(FVector3(0.0f, 1.0f, 0.0f), Math::Radians(Degrees));
    }
}

TEST(NetTransformRecord, AFarRecordKeepsTheHeadingAroundTheWholeCircle)
{
    constexpr float FarYawStepDegrees = 360.0f / 256.0f;
    for (const float Degrees : { -170.0f, -135.0f, -90.0f, 0.0f, 45.0f, 135.0f, 170.0f })
    {
        const float Decoded = Math::Degrees(Math::YawFirstEulerAngles(RoundTrip(ENetLODTier::Far, YawQuat(Degrees))).y);
        const float Error = std::fmod(Decoded - Degrees + 540.0f, 360.0f) - 180.0f;
        EXPECT_LE(Math::Abs(Error), FarYawStepDegrees) << "yaw " << Degrees << " decoded as " << Decoded;
    }
}

TEST(NetTransformRecord, NearAndMidRecordsRoundTripYawOnlyAndFullRotations)
{
    const FQuat Tilted = Math::FromAxisAngle(Math::Normalize(FVector3(1.0f, 2.0f, -0.5f)), 2.3f);
    for (const ENetLODTier Tier : { ENetLODTier::Near, ENetLODTier::Mid })
    {
        for (const FQuat& Rot : { YawQuat(135.0f), YawQuat(-135.0f), Tilted })
        {
            EXPECT_TRUE(Math::IsSameRotation(RoundTrip(Tier, Rot), Rot, 1.0e-4f)) << "tier " << static_cast<int>(Tier);
        }
    }
}
