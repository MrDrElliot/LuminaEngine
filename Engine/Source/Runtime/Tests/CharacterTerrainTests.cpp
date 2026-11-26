#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Physics/Physics.h"
#include "Physics/PhysicsScene.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"

using namespace Lumina;

namespace
{
    void BindTerrainTestTransform(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
        Transform.Bind(Registry, Entity);
        Transform.ResetDirtyState();
    }

    using FHeightFn = float (*)(float X, float Z);

    // A world with one terrain and the real character path, stepped the way an engine frame steps it.
    struct FTerrainWorld
    {
        CWorld* World = nullptr;
        TStrongObjectPtr<CWorld> WorldOwner;

        int32    Resolution = 0;
        float    TileWorldSize = 0.0f;
        float    MaxHeight = 0.0f;
        FVector3 Origin = FVector3(0.0f);
        TVector<float> Heights;

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }
        Physics::IPhysicsScene& Scene() const { return *World->GetPhysicsScene(); }

        FTerrainWorld()
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, FName("CharacterTerrainTestWorld"), FGuid::New(), OF_Transient);
            WorldOwner = World;
            ECS::FRegistry& R = Registry();
            R.Ctx().Emplace<CWorld*>(World);
            R.GetSignals<STransformComponent>().OnConstruct.Connect<&BindTerrainTestTransform>();
            if (Physics::GetPhysicsContext() == nullptr)
            {
                Physics::Initialize();
            }
            World->EnsurePhysicsScene();
        }

        ~FTerrainWorld()
        {
            WorldOwner.Reset();
        }

        ECS::FEntity SpawnTerrain(int32 InResolution, float InTileWorldSize, float InMaxHeight, const FVector3& InOrigin, FHeightFn Height,
                                  const FQuat& Rotation = FQuat(1.0f, 0.0f, 0.0f, 0.0f))
        {
            Resolution = InResolution;
            TileWorldSize = InTileWorldSize;
            MaxHeight = InMaxHeight;
            Origin = InOrigin;

            ECS::FRegistry& R = Registry();
            const ECS::FEntity Entity = R.Create();
            STransformComponent& Transform = R.Emplace<STransformComponent>(Entity);
            Transform.SetLocalLocation(Origin);
            Transform.SetLocalRotation(Rotation);

            STerrainComponent Terrain;
            Terrain.Resolution = Resolution;
            Terrain.TileWorldSize = TileWorldSize;
            Terrain.MaxHeight = MaxHeight;
            Terrain.Heightmap.SetNum(Resolution * Resolution);
            Heights.SetNum(Resolution * Resolution);
            const float Stride = TileWorldSize / float(Resolution - 1);
            for (int32 Z = 0; Z < Resolution; ++Z)
            {
                for (int32 X = 0; X < Resolution; ++X)
                {
                    const float H = Height(-TileWorldSize * 0.5f + X * Stride, -TileWorldSize * 0.5f + Z * Stride);
                    Terrain.Heightmap[Z * Resolution + X] = H / MaxHeight;
                    Heights[Z * Resolution + X] = H;
                }
            }
            R.Emplace<STerrainComponent>(Entity, std::move(Terrain));
            R.Emplace<STerrainColliderComponent>(Entity);
            return Entity;
        }

        // The surface the renderer draws, split corner to opposite corner the way TerrainTriangleCorners does.
        float RenderedHeight(float WorldX, float WorldZ, FVector3* OutNormal = nullptr) const
        {
            const float Stride = TileWorldSize / float(Resolution - 1);
            const float Fx = Math::Clamp((WorldX - Origin.x + TileWorldSize * 0.5f) / Stride, 0.0f, float(Resolution - 1) - 1e-4f);
            const float Fz = Math::Clamp((WorldZ - Origin.z + TileWorldSize * 0.5f) / Stride, 0.0f, float(Resolution - 1) - 1e-4f);
            const int32 X0 = (int32)Fx;
            const int32 Z0 = (int32)Fz;
            const float Tx = Fx - X0;
            const float Tz = Fz - Z0;
            const float H00 = Heights[Z0 * Resolution + X0];
            const float H10 = Heights[Z0 * Resolution + X0 + 1];
            const float H01 = Heights[(Z0 + 1) * Resolution + X0];
            const float H11 = Heights[(Z0 + 1) * Resolution + X0 + 1];

            float H, Gx, Gz;
            if (Tx >= Tz)
            {
                H  = H00 + Tx * (H10 - H00) + Tz * (H11 - H10);
                Gx = (H10 - H00) / Stride;
                Gz = (H11 - H10) / Stride;
            }
            else
            {
                H  = H00 + Tz * (H01 - H00) + Tx * (H11 - H01);
                Gx = (H11 - H01) / Stride;
                Gz = (H01 - H00) / Stride;
            }
            if (OutNormal != nullptr)
            {
                *OutNormal = Math::Normalize(FVector3(-Gx, 1.0f, -Gz));
            }
            return Origin.y + H;
        }

        ECS::FEntity SpawnCharacter(const FVector3& Location)
        {
            ECS::FRegistry& R = Registry();
            const ECS::FEntity Entity = R.Create();
            R.Emplace<STransformComponent>(Entity).SetLocalLocation(Location);
            R.Emplace<SCharacterPhysicsComponent>(Entity);
            R.Emplace<SCharacterMovementComponent>(Entity);
            R.Emplace<SCharacterControllerComponent>(Entity);
            return Entity;
        }

        FVector3 Location(ECS::FEntity Entity) const
        {
            return ECS::Utils::ComputeWorldTransform(Registry(), Entity).GetLocation();
        }

        void Frame(double Dt)
        {
            ECS::Utils::ResolveAllDirtyTransforms(Registry());
            Scene().Update(Dt);
            Scene().DispatchPendingEvents();
            ECS::Utils::ResolveAllDirtyTransforms(Registry());
            World->FlushCommands();
            World->GetEvents().Advance();
        }
    };

    constexpr double kDt = 1.0 / 60.0;

    struct FWalkResult
    {
        float Traveled = 0.0f;
        float WorstGap = 0.0f;
        float MeanGap = 0.0f;
        float GroundedFraction = 0.0f;
    };

    // Walks the character along Direction, measuring how far the capsule sits from the rendered surface under it.
    FWalkResult Walk(FTerrainWorld& W, ECS::FEntity Character, const FVector3& Direction, int32 Frames)
    {
        ECS::FRegistry& R = W.Registry();
        const SCharacterPhysicsComponent& Physics = R.Get<SCharacterPhysicsComponent>(Character);
        const float Radius = Physics.Radius;
        const float HalfHeight = Physics.HalfHeight;

        FWalkResult Out;
        const FVector3 Start = W.Location(Character);
        int32 Grounded = 0;
        double GapSum = 0.0;
        for (int32 Frame = 0; Frame < Frames; ++Frame)
        {
            R.Get<SCharacterControllerComponent>(Character).AddWorldMovementInput(Direction);
            W.Frame(kDt);

            const FVector3 P = W.Location(Character);
            FVector3 Normal;
            const float Surface = W.RenderedHeight(P.x, P.z, &Normal);
            // A sphere resting on a plane has its center Radius along the normal, which is Radius / n.y straight up.
            const float Expected = Surface + HalfHeight + Radius / Math::Max(Normal.y, 0.2f);
            const float Gap = P.y - Expected;
            if (Frame > 30)
            {
                Out.WorstGap = Math::Max(Out.WorstGap, Math::Abs(Gap));
                GapSum += Math::Abs(Gap);
            }
            Grounded += R.Get<SCharacterMovementComponent>(Character).bGrounded ? 1 : 0;
        }
        const FVector3 End = W.Location(Character);
        Out.Traveled = Math::Length(FVector3(End.x - Start.x, 0.0f, End.z - Start.z));
        Out.MeanGap = (float)(GapSum / Math::Max(Frames - 31, 1));
        Out.GroundedFraction = float(Grounded) / float(Frames);
        return Out;
    }

    float Flat(float, float) { return 2.0f; }

    // Rolling hills that stay well inside a walkable slope.
    float Hills(float X, float Z) { return 6.0f + 3.0f * std::sin(X * 0.08f) * std::cos(Z * 0.07f) + 1.0f * std::sin(X * 0.21f + Z * 0.17f); }

    // Every quad twisted, so the two diagonal splits disagree by the full amplitude at each quad center.
    float Twisted(float X, float Z)
    {
        const int32 I = (int32)std::lround(X / 4.0f);
        const int32 J = (int32)std::lround(Z / 4.0f);
        return ((I + J) & 1) ? 1.5f : 0.0f;
    }

    void Report(const char* Name, const FWalkResult& R)
    {
        std::printf("[%s] traveled %.2f m, worst gap %.3f m, mean gap %.3f m, grounded %.0f%%\n",
            Name, R.Traveled, R.WorstGap, R.MeanGap, R.GroundedFraction * 100.0f);
    }
}

TEST(CharacterTerrain, WalksAcrossFlatTerrain)
{
    FTerrainWorld W;
    W.SpawnTerrain(65, 256.0f, 16.0f, FVector3(0.0f), &Flat);
    const ECS::FEntity Character = W.SpawnCharacter(FVector3(0.0f, 5.0f, 0.0f));
    for (int32 i = 0; i < 60; ++i) { W.Frame(kDt); }

    const FWalkResult R = Walk(W, Character, FVector3(1.0f, 0.0f, 0.0f), 300);
    Report("flat", R);
    EXPECT_GT(R.Traveled, 5.0f * 5.0f * 0.85f) << "the character did not cover the distance its speed allows";
    EXPECT_LT(R.WorstGap, 0.08f) << "the capsule left the rendered surface";
    EXPECT_GT(R.GroundedFraction, 0.97f);
}

TEST(CharacterTerrain, WalksOverRollingHills)
{
    FTerrainWorld W;
    W.SpawnTerrain(129, 256.0f, 16.0f, FVector3(0.0f), &Hills);
    const ECS::FEntity Character = W.SpawnCharacter(FVector3(-40.0f, W.RenderedHeight(-40.0f, 3.0f) + 3.0f, 3.0f));
    for (int32 i = 0; i < 60; ++i) { W.Frame(kDt); }

    const FWalkResult R = Walk(W, Character, Math::Normalize(FVector3(1.0f, 0.0f, 0.35f)), 600);
    Report("hills", R);
    EXPECT_GT(R.Traveled, 5.0f * 10.0f * 0.85f);
    EXPECT_LT(R.WorstGap, 0.15f);
    EXPECT_GT(R.GroundedFraction, 0.95f);
}

TEST(CharacterTerrain, StandsOnTheRenderedSurfaceOfTwistedQuads)
{
    FTerrainWorld W;
    W.SpawnTerrain(33, 128.0f, 4.0f, FVector3(0.0f), &Twisted);
    const ECS::FEntity Character = W.SpawnCharacter(FVector3(-30.0f, 5.0f, 2.0f));
    for (int32 i = 0; i < 60; ++i) { W.Frame(kDt); }

    const FWalkResult R = Walk(W, Character, Math::Normalize(FVector3(1.0f, 0.0f, 0.2f)), 480);
    Report("twisted", R);
    EXPECT_LT(R.WorstGap, 0.2f) << "collision and the rendered terrain split their quads along different diagonals";
}

TEST(CharacterTerrain, OffsetTerrainCollidesWhereItIsDrawn)
{
    FTerrainWorld W;
    W.SpawnTerrain(65, 128.0f, 16.0f, FVector3(300.0f, -20.0f, -150.0f), &Hills);
    const ECS::FEntity Character = W.SpawnCharacter(FVector3(290.0f, W.RenderedHeight(290.0f, -140.0f) + 3.0f, -140.0f));
    for (int32 i = 0; i < 60; ++i) { W.Frame(kDt); }

    const FWalkResult R = Walk(W, Character, FVector3(0.0f, 0.0f, 1.0f), 300);
    Report("offset", R);
    EXPECT_GT(R.Traveled, 5.0f * 5.0f * 0.85f);
    EXPECT_LT(R.WorstGap, 0.15f);
}

TEST(CharacterTerrain, RotatedTerrainCollidesWhereItIsDrawn)
{
    // The renderer draws terrain axis aligned whatever the entity's rotation, so collision must too.
    FTerrainWorld W;
    W.SpawnTerrain(65, 128.0f, 16.0f, FVector3(0.0f), &Hills, FQuat(FVector3(0.0f, Math::Radians(30.0f), 0.0f)));
    const ECS::FEntity Character = W.SpawnCharacter(FVector3(10.0f, W.RenderedHeight(10.0f, 10.0f) + 3.0f, 10.0f));
    for (int32 i = 0; i < 60; ++i) { W.Frame(kDt); }

    const FWalkResult R = Walk(W, Character, FVector3(-1.0f, 0.0f, 0.0f), 240);
    Report("rotated", R);
    EXPECT_LT(R.WorstGap, 0.15f);
}

TEST(CharacterTerrain, StepsFromTerrainOntoAStaticBoxAndBack)
{
    FTerrainWorld W;
    W.SpawnTerrain(65, 128.0f, 16.0f, FVector3(0.0f), &Flat);

    // A slab whose top sits a step's height above the terrain, the kind of curb a level puts down.
    ECS::FRegistry& R = W.Registry();
    const ECS::FEntity Slab = R.Create();
    R.Emplace<STransformComponent>(Slab).SetLocalLocation(FVector3(10.0f, 2.0f + 0.25f - 0.5f, 0.0f));
    R.Emplace<SRigidBodyComponent>(Slab).BodyType = EBodyType::Static;
    R.Emplace<SBoxColliderComponent>(Slab).HalfExtent = FVector3(4.0f, 0.5f, 4.0f);

    const ECS::FEntity Character = W.SpawnCharacter(FVector3(0.0f, 5.0f, 0.0f));
    for (int32 i = 0; i < 60; ++i) { W.Frame(kDt); }

    const SCharacterPhysicsComponent& Physics = R.Get<SCharacterPhysicsComponent>(Character);
    const float Rest = Physics.HalfHeight + Physics.Radius;

    for (int32 i = 0; i < 150; ++i)
    {
        R.Get<SCharacterControllerComponent>(Character).AddWorldMovementInput(FVector3(1.0f, 0.0f, 0.0f));
        W.Frame(kDt);
    }
    const FVector3 OnSlab = W.Location(Character);
    std::printf("[slab] at x %.2f y %.2f, slab top 2.25\n", OnSlab.x, OnSlab.y);
    EXPECT_GT(OnSlab.x, 7.0f) << "the character could not step from terrain onto the slab";
    EXPECT_NEAR(OnSlab.y, 2.25f + Rest, 0.1f);

    for (int32 i = 0; i < 200; ++i)
    {
        R.Get<SCharacterControllerComponent>(Character).AddWorldMovementInput(FVector3(1.0f, 0.0f, 0.0f));
        W.Frame(kDt);
    }
    const FVector3 Past = W.Location(Character);
    std::printf("[slab] past it at x %.2f y %.2f\n", Past.x, Past.y);
    EXPECT_GT(Past.x, 16.0f) << "the character did not walk off the slab back onto terrain";
    EXPECT_NEAR(Past.y, 2.0f + Rest, 0.1f);
    EXPECT_TRUE(R.Get<SCharacterMovementComponent>(Character).bGrounded);
}

TEST(CharacterTerrain, WalksOffTheTerrainEdgeAndFalls)
{
    FTerrainWorld W;
    W.SpawnTerrain(17, 32.0f, 16.0f, FVector3(0.0f), &Flat);
    const ECS::FEntity Character = W.SpawnCharacter(FVector3(10.0f, 5.0f, 0.0f));
    for (int32 i = 0; i < 60; ++i) { W.Frame(kDt); }

    for (int32 i = 0; i < 180; ++i)
    {
        W.Registry().Get<SCharacterControllerComponent>(Character).AddWorldMovementInput(FVector3(1.0f, 0.0f, 0.0f));
        W.Frame(kDt);
    }
    const FVector3 P = W.Location(Character);
    std::printf("[edge] at x %.2f y %.2f\n", P.x, P.y);
    EXPECT_GT(P.x, 16.5f) << "an invisible wall held the character at the terrain edge";
    EXPECT_LT(P.y, 0.0f) << "the character did not fall past the edge";
}

TEST(CharacterTerrain, StepsFromABoxFloorOntoAStaticBox)
{
    // The same curb on box ground, which separates a terrain-specific step failure from a general one.
    FTerrainWorld W;
    ECS::FRegistry& R = W.Registry();
    const ECS::FEntity Floor = R.Create();
    R.Emplace<STransformComponent>(Floor).SetLocalLocation(FVector3(0.0f, 1.5f, 0.0f));
    R.Emplace<SRigidBodyComponent>(Floor).BodyType = EBodyType::Static;
    R.Emplace<SBoxColliderComponent>(Floor).HalfExtent = FVector3(64.0f, 0.5f, 64.0f);

    const ECS::FEntity Slab = R.Create();
    R.Emplace<STransformComponent>(Slab).SetLocalLocation(FVector3(10.0f, 2.0f + 0.25f - 0.5f, 0.0f));
    R.Emplace<SRigidBodyComponent>(Slab).BodyType = EBodyType::Static;
    R.Emplace<SBoxColliderComponent>(Slab).HalfExtent = FVector3(4.0f, 0.5f, 4.0f);

    const ECS::FEntity Character = W.SpawnCharacter(FVector3(0.0f, 5.0f, 0.0f));
    for (int32 i = 0; i < 60; ++i) { W.Frame(kDt); }
    for (int32 i = 0; i < 150; ++i)
    {
        R.Get<SCharacterControllerComponent>(Character).AddWorldMovementInput(FVector3(1.0f, 0.0f, 0.0f));
        W.Frame(kDt);
    }
    const FVector3 OnSlab = W.Location(Character);
    std::printf("[box slab] at x %.2f y %.2f\n", OnSlab.x, OnSlab.y);
    EXPECT_GT(OnSlab.x, 7.0f) << "the character could not step from box ground onto the slab";
}

TEST(CharacterTerrain, CollisionFollowsAHeightmapEdit)
{
    FTerrainWorld W;
    const ECS::FEntity TerrainEntity = W.SpawnTerrain(65, 128.0f, 16.0f, FVector3(0.0f), &Flat);
    const ECS::FEntity Character = W.SpawnCharacter(FVector3(0.0f, 5.0f, 0.0f));
    for (int32 i = 0; i < 60; ++i) { W.Frame(kDt); }

    // Raise the whole tile by a meter, the way a sculpt stroke rewrites samples and bumps the version.
    STerrainComponent& Terrain = W.Registry().Get<STerrainComponent>(TerrainEntity);
    for (int32 i = 0; i < Terrain.Heightmap.Num(); ++i)
    {
        Terrain.Heightmap[i] += 1.0f / Terrain.MaxHeight;
        W.Heights[i] += 1.0f;
    }
    Terrain.MarkHeightDirty(FIntVector2(0, 0), FIntVector2(Terrain.Resolution - 1, Terrain.Resolution - 1));
    for (int32 i = 0; i < 90; ++i) { W.Frame(kDt); }

    const SCharacterPhysicsComponent& Physics = W.Registry().Get<SCharacterPhysicsComponent>(Character);
    const FVector3 P = W.Location(Character);
    std::printf("[edit] character y %.2f, surface %.2f\n", P.y, W.RenderedHeight(P.x, P.z));
    EXPECT_NEAR(P.y, W.RenderedHeight(P.x, P.z) + Physics.HalfHeight + Physics.Radius, 0.1f) << "collision kept the heightmap from before the edit";
}

TEST(CharacterTerrain, MovedTerrainKeepsItsCollisionUnderTheDrawnSurface)
{
    FTerrainWorld W;
    const ECS::FEntity TerrainEntity = W.SpawnTerrain(65, 128.0f, 16.0f, FVector3(0.0f), &Hills);
    for (int32 i = 0; i < 10; ++i) { W.Frame(kDt); }

    // The editor moving the terrain entity, which used to put the body at the entity origin and lose the half-tile shift.
    W.Origin = FVector3(20.0f, 3.0f, -10.0f);
    W.Registry().Get<STransformComponent>(TerrainEntity).SetLocalLocation(W.Origin);
    for (int32 i = 0; i < 10; ++i) { W.Frame(kDt); }

    const ECS::FEntity Character = W.SpawnCharacter(FVector3(30.0f, W.RenderedHeight(30.0f, 0.0f) + 3.0f, 0.0f));
    for (int32 i = 0; i < 60; ++i) { W.Frame(kDt); }

    const FWalkResult R = Walk(W, Character, FVector3(0.0f, 0.0f, 1.0f), 240);
    Report("moved", R);
    EXPECT_LT(R.WorstGap, 0.15f);
}
