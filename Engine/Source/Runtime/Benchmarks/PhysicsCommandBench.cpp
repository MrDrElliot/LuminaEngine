#include <gtest/gtest.h>

#include "Containers/Algorithm.h"
#include "Containers/Function.h"
#include "Core/Object/ObjectCore.h"
#include "Physics/API/Box3D/Box3DPhysicsScene.h"
#include "Physics/API/Box3D/Box3DRagdollHandle.h"
#include "Platform/Time/PlatformTime.h"
#include "Renderer/SkeletonResource.h"
#include "World/ECS/EntityMap.h"
#include "World/World.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"
#include <cstdio>

using namespace Lumina;

namespace
{
    constexpr int32 kRigidBodies = 5000;
    constexpr int32 kRagdolls    = 500;
    constexpr int32 kWarmFrames  = 10;
    constexpr int32 kFrames      = 60;

    struct FBenchBone
    {
        const char* Name;
        int32       Parent;
        FVector3    Offset;
    };

    // A 13-limb humanoid, offsets from each parent in meters.
    constexpr FBenchBone kHumanoid[] =
    {
        { "pelvis",     -1, FVector3( 0.00f, 1.00f, 0.0f) },
        { "spine",       0, FVector3( 0.00f, 0.25f, 0.0f) },
        { "chest",       1, FVector3( 0.00f, 0.25f, 0.0f) },
        { "head",        2, FVector3( 0.00f, 0.30f, 0.0f) },
        { "upperarm_l",  2, FVector3( 0.20f, 0.15f, 0.0f) },
        { "lowerarm_l",  4, FVector3( 0.30f, 0.00f, 0.0f) },
        { "upperarm_r",  2, FVector3(-0.20f, 0.15f, 0.0f) },
        { "lowerarm_r",  6, FVector3(-0.30f, 0.00f, 0.0f) },
        { "thigh_l",     0, FVector3( 0.12f, -0.05f, 0.0f) },
        { "calf_l",      8, FVector3( 0.00f, -0.45f, 0.0f) },
        { "thigh_r",     0, FVector3(-0.12f, -0.05f, 0.0f) },
        { "calf_r",     10, FVector3( 0.00f, -0.45f, 0.0f) },
        { "foot_l",      9, FVector3( 0.00f, -0.45f, 0.1f) },
    };

    class FBenchScene
    {
    public:

        FBenchScene()
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            Registry = &World->GetEntityRegistry();
            Scene = MakeUnique<Physics::FBox3DPhysicsScene>(World);
            Scene->Simulate();
            Dt = 1.0f / Math::Max(10.0f, World->GetDefaultWorldSettings().PhysicsHz);

            const int32 NumBones = (int32)std::size(kHumanoid);
            TVector<FMatrix4> Globals(NumBones);
            for (int32 i = 0; i < NumBones; ++i)
            {
                const FMatrix4 Parent = kHumanoid[i].Parent >= 0 ? Globals[kHumanoid[i].Parent] : FMatrix4(1.0f);
                Globals[i] = Math::Translate(Parent, kHumanoid[i].Offset);

                FSkeletonResource::FBoneInfo Bone;
                Bone.Name = FName(kHumanoid[i].Name);
                Bone.ParentIndex = kHumanoid[i].Parent >= 0 ? kHumanoid[i].Parent : Constants::kIndexNone;
                Bone.LocalTransform = Math::Translate(FMatrix4(1.0f), kHumanoid[i].Offset);
                Bone.InvBindMatrix = Math::Inverse(Globals[i]);
                Skeleton.BoneNameToIndex[Bone.Name] = i;
                Skeleton.Bones.Add(Bone);
            }
            BindGlobals = Move(Globals);
        }

        ~FBenchScene()
        {
            Ragdolls.Reset();
            Scene->StopSimulate();
            Scene.reset();
        }

        FBenchScene(const FBenchScene&) = delete;
        FBenchScene& operator=(const FBenchScene&) = delete;
        FBenchScene(FBenchScene&&) = delete;
        FBenchScene& operator=(FBenchScene&&) = delete;

        // Spread on a grid far enough apart that nothing touches, and drifting so no island sleeps.
        void AddRigidBodies(int32 Count)
        {
            for (int32 i = 0; i < Count; ++i)
            {
                const ECS::FEntity Entity = Registry->Create();
                Registry->Emplace<STransformComponent>(Entity).SetLocation(FVector3((float)(i % 100) * 2.0f, 500.0f, (float)(i / 100) * 2.0f));
                auto& Body = Registry->Emplace<SRigidBodyComponent>(Entity);
                Body.bUseGravity = false;
                Body.LinearDamping = 0.0f;
                Registry->Emplace<SSphereColliderComponent>(Entity).Radius = 0.25f;
                Bodies.Add(Entity);
            }
            Scene->Update(0.0);
            for (ECS::FEntity Entity : Bodies)
            {
                Scene->SetLinearVelocity(Entity, FVector3(0.1f, 0.0f, 0.0f));
            }
        }

        void AddRagdolls(int32 Count)
        {
            for (int32 i = 0; i < Count; ++i)
            {
                const ECS::FEntity Owner = Registry->Create();
                Physics::FRagdollDesc Desc;
                Desc.Entity = Owner;
                Desc.Skeleton = &Skeleton;
                Desc.ComponentBoneGlobals = &BindGlobals;
                Desc.EntityToWorld = Math::Translate(FMatrix4(1.0f), FVector3((float)(i % 25) * 4.0f, 0.0f, (float)(i / 25) * 4.0f));
                Desc.CollisionGroupID = Scene->AllocateRagdollGroupID();
                Ragdolls.Add(Scene->CreateRagdoll(Desc));
                RagdollOwners.Add(Owner);
            }
            Scene->Update(0.0);
        }

        // Frames alternate without and with the work, so drift in the step itself cancels out of the difference.
        struct FInterleaved
        {
            double StepMs = 0.0;
            double WithWorkMs = 0.0;
            double QueueUs = 0.0;
        };

        template<typename TPerFrame>
        FInterleaved CompareFrames(TPerFrame&& PerFrame)
        {
            for (int32 Frame = 0; Frame < kWarmFrames; ++Frame)
            {
                PerFrame();
                Scene->Update(Dt);
            }
            TVector<double> Plain, Worked, Queue;
            for (int32 Frame = 0; Frame < kFrames; ++Frame)
            {
                uint64 Start = PlatformTime::Cycles();
                Scene->Update(Dt);
                Plain.Add(PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start));

                Start = PlatformTime::Cycles();
                PerFrame();
                const uint64 Queued = PlatformTime::Cycles();
                Scene->Update(Dt);
                Worked.Add(PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start));
                Queue.Add(PlatformTime::ToMilliseconds(Queued - Start) * 1000.0);
            }
            Algo::Sort(Plain);
            Algo::Sort(Worked);
            Algo::Sort(Queue);
            return { Plain[Plain.Num() / 2], Worked[Worked.Num() / 2], Queue[Queue.Num() / 2] };
        }

        // Median of one pass of Work over every ragdoll, in microseconds, with no step in between.
        template<typename TWork>
        double MedianPassUs(TWork&& Work)
        {
            TVector<double> Times;
            for (int32 Pass = 0; Pass < kFrames; ++Pass)
            {
                const uint64 Start = PlatformTime::Cycles();
                Work();
                Times.Add(PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start) * 1000.0);
            }
            Algo::Sort(Times);
            return Times[Times.Num() / 2];
        }

        CWorld* World = nullptr;
        ECS::FRegistry* Registry = nullptr;
        TUniquePtr<Physics::FBox3DPhysicsScene> Scene;
        float Dt = 0.0f;
        FSkeletonResource Skeleton;
        TVector<FMatrix4> BindGlobals;
        TVector<ECS::FEntity> Bodies;
        TVector<ECS::FEntity> RagdollOwners;
        TVector<TSharedPtr<FPhysicsRagdollHandle>> Ragdolls;
    };
}

TEST(PhysicsCommandBench, RigidBodyCommandsWithRagdollsPresent)
{
    for (const int32 NumRagdolls : { 0, kRagdolls })
    {
        FBenchScene Bench;
        Bench.AddRigidBodies(kRigidBodies);
        Bench.AddRagdolls(NumRagdolls);
        const FBenchScene::FInterleaved Result = Bench.CompareFrames([&Bench]
        {
            for (ECS::FEntity Entity : Bench.Bodies) { Bench.Scene->AddImpulse(Entity, FVector3(0.0f, 0.0f, 1e-4f)); }
        });
        std::printf("  %d rigid impulses, %3d ragdolls   step %6.3f ms   +commands %6.3f ms   queue %5.1f ns   queue+apply %5.1f ns each\n",
            kRigidBodies, NumRagdolls, Result.StepMs, Result.WithWorkMs, Result.QueueUs * 1000.0 / kRigidBodies,
            (Result.WithWorkMs - Result.StepMs) * 1e6 / kRigidBodies);
    }
}

TEST(PhysicsCommandBench, WholeRagdollAndPerLimbCalls)
{
    FBenchScene Bench;
    Bench.AddRagdolls(kRagdolls);
    const TVector<ECS::FEntity>& Owners = Bench.RagdollOwners;
    Physics::FBox3DPhysicsScene& Scene = *Bench.Scene;

    TVector<SPhysicsBodyTarget> Heads(Owners.Num());
    const double LimbLookupUs = Bench.MedianPassUs([&]
    {
        for (int32 i = 0; i < Owners.Num(); ++i) { Heads[i] = Scene.GetRagdollBoneTarget(Owners[i], FName("head")); }
    });
    FVector3 Sink(0.0f);
    const double ReadUs = Bench.MedianPassUs([&] { for (ECS::FEntity E : Owners) { Sink += Scene.GetLinearVelocity(E); } });
    EXPECT_TRUE(Heads[0].IsSet());

    struct FCase { const char* Label; TFunction<void()> Work; };
    const FCase Cases[] =
    {
        { "SetLinearVelocity on the entity   ", [&] { for (ECS::FEntity E : Owners) { Scene.SetLinearVelocity(E, FVector3(0.5f, 0.0f, 0.0f)); } } },
        { "AddImpulse on the entity          ", [&] { for (ECS::FEntity E : Owners) { Scene.AddImpulse(E, FVector3(0.0f, 0.0f, 0.5f)); } } },
        { "AddImpulseAtPosition on the entity", [&] { for (ECS::FEntity E : Owners) { Scene.AddImpulseAtPosition(E, FVector3(0.0f, 0.0f, 0.5f), FVector3(0.0f)); } } },
        { "SetTargetLinearVelocity on a limb ", [&] { for (const SPhysicsBodyTarget& Head : Heads) { Scene.SetTargetLinearVelocity(Head, FVector3(0.0f, 2.0f, 0.0f)); } } },
    };

    std::printf("\n  %d ragdolls x %d limbs, one call per ragdoll per frame\n", kRagdolls, (int)std::size(kHumanoid));
    for (const FCase& Case : Cases)
    {
        const FBenchScene::FInterleaved Result = Bench.CompareFrames(Case.Work);
        std::printf("  %s   step %6.3f ms   +calls %6.3f ms   queue %5.1f ns   queue+apply %6.1f ns per ragdoll\n", Case.Label,
            Result.StepMs, Result.WithWorkMs, Result.QueueUs * 1000.0 / kRagdolls, (Result.WithWorkMs - Result.StepMs) * 1e6 / kRagdolls);
    }
    std::printf("  GetRagdollBoneTarget by name        %6.1f ns each\n", LimbLookupUs * 1000.0 / kRagdolls);
    std::printf("  GetLinearVelocity on the entity     %6.1f ns each (%.1f)\n\n", ReadUs * 1000.0 / kRagdolls, Sink.x);
}

// The lookup every entity command now makes before its body record, at the bench's sizes.
TEST(PhysicsCommandBench, RagdollLookupMissPerCommand)
{
    ECS::FRegistry Registry;
    TVector<ECS::FEntity> Entities;
    ECS::TEntityMap<void*> Ragdolls;
    for (int32 i = 0; i < kRigidBodies + kRagdolls; ++i)
    {
        Entities.Add(Registry.Create());
    }
    for (int32 i = kRigidBodies; i < kRigidBodies + kRagdolls; ++i)
    {
        Ragdolls.FindOrAdd(Entities[i]) = &Registry;
    }

    TVector<double> Times;
    uint64 Hits = 0;
    for (int32 Pass = 0; Pass < kFrames; ++Pass)
    {
        const uint64 Start = PlatformTime::Cycles();
        for (int32 i = 0; i < kRigidBodies; ++i)
        {
            Hits += Ragdolls.Find(Entities[i]) != nullptr;
        }
        Times.Add(PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start) * 1e6 / kRigidBodies);
    }
    Algo::Sort(Times);
    EXPECT_EQ(Hits, 0u);
    std::printf("\n  ragdoll map miss for a rigid body command  %5.2f ns\n\n", Times[Times.Num() / 2]);
}
