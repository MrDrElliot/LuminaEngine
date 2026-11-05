#include <gtest/gtest.h>

#include <box3d/box3d.h>
#include <box3d/collision.h>

#include "Physics/API/Box3D/Box3DTaskBridge.h"
#include "Physics/API/Box3D/Box3DUtils.h"
#include "Physics/API/Box3D/Box3DInternal.h"
#include "Assets/AssetTypes/Mesh/MeshCollision.h"
#include "Physics/CollisionGen.h"

using namespace Lumina;

namespace
{
    // Mirrors how FBox3DPhysicsScene builds its world, so the bridge is exercised the way the engine uses it.
    struct FTestWorld
    {
        Physics::FBox3DTaskBridge Bridge;
        b3WorldId WorldId{};

        explicit FTestWorld(bool bMultithreaded = true)
        {
            b3WorldDef Def = b3DefaultWorldDef();
            if (bMultithreaded)
            {
                Def.workerCount = Bridge.GetWorkerCount();
                Def.enqueueTask = &Physics::FBox3DTaskBridge::EnqueueTask;
                Def.finishTask = &Physics::FBox3DTaskBridge::FinishTask;
                Def.userTaskContext = &Bridge;
            }
            else
            {
                Def.workerCount = 1;
            }

            WorldId = b3CreateWorld(&Def);
        }

        ~FTestWorld()
        {
            if (b3World_IsValid(WorldId))
            {
                b3DestroyWorld(WorldId);
            }
        }

        FTestWorld(const FTestWorld&) = delete;
        FTestWorld& operator=(const FTestWorld&) = delete;
    };

    b3HullData* MakeBoxHull(float HX, float HY, float HZ)
    {
        const b3Vec3 Points[8] =
        {
            { -HX, -HY, -HZ }, {  HX, -HY, -HZ }, { -HX,  HY, -HZ }, {  HX,  HY, -HZ },
            { -HX, -HY,  HZ }, {  HX, -HY,  HZ }, { -HX,  HY,  HZ }, {  HX,  HY,  HZ },
        };
        return b3CreateHull(Points, 8, B3_MAX_HULL_VERTICES);
    }

    // The seat probe filters by profile, so a raw box3d shape needs the user data the engine stamps.
    b3ShapeDef StaticGroundShapeDef()
    {
        SCollisionProfile Ground;
        Ground.Layer = ECollisionProfiles::Static;
        Ground.Mask = ECollisionProfiles::Static | ECollisionProfiles::Dynamic;

        b3ShapeDef Def = b3DefaultShapeDef();
        Def.userData = Box3DUtils::PackProfileUserData(Ground);
        return Def;
    }

    b3BodyId AddGround(b3WorldId WorldId, b3HullData* Hull)
    {
        b3BodyDef BodyDef = b3DefaultBodyDef();
        BodyDef.type = b3_staticBody;
        BodyDef.position = b3Vec3{ 0.0f, 0.0f, 0.0f };

        const b3BodyId Body = b3CreateBody(WorldId, &BodyDef);
        b3ShapeDef ShapeDef = StaticGroundShapeDef();
        b3CreateHullShape(Body, &ShapeDef, Hull);
        return Body;
    }

    b3BodyId AddFallingSphere(b3WorldId WorldId, float Height, float Radius)
    {
        b3BodyDef BodyDef = b3DefaultBodyDef();
        BodyDef.type = b3_dynamicBody;
        BodyDef.position = b3Vec3{ 0.0f, Height, 0.0f };

        const b3BodyId Body = b3CreateBody(WorldId, &BodyDef);

        b3ShapeDef ShapeDef = b3DefaultShapeDef();
        const b3Sphere Sphere{ b3Vec3{ 0.0f, 0.0f, 0.0f }, Radius };
        b3CreateSphereShape(Body, &ShapeDef, &Sphere);
        b3Body_ApplyMassFromShapes(Body);
        return Body;
    }
}

TEST(Box3DPhysics, VendoredLibraryReportsSinglePrecision)
{
    const b3Version Version = b3GetVersion();
    EXPECT_GE(Version.major, 0);
    EXPECT_FALSE(b3IsDoublePrecision());
}

TEST(Box3DPhysics, VectorAndQuaternionRoundTrip)
{
    const FVector3 Vector(1.5f, -2.25f, 3.75f);
    const FVector3 BackVector = Box3DUtils::FromB3Vec3(Box3DUtils::ToB3Vec3(Vector));
    EXPECT_FLOAT_EQ(BackVector.x, Vector.x);
    EXPECT_FLOAT_EQ(BackVector.y, Vector.y);
    EXPECT_FLOAT_EQ(BackVector.z, Vector.z);

    const FQuat Rotation = Math::Normalize(FQuat(0.5f, 0.5f, -0.5f, 0.5f));
    const FQuat BackRotation = Box3DUtils::FromB3Quat(Box3DUtils::ToB3Quat(Rotation));
    EXPECT_NEAR(BackRotation.x, Rotation.x, 1e-6f);
    EXPECT_NEAR(BackRotation.y, Rotation.y, 1e-6f);
    EXPECT_NEAR(BackRotation.z, Rotation.z, 1e-6f);
    EXPECT_NEAR(BackRotation.w, Rotation.w, 1e-6f);
}

TEST(Box3DPhysics, RotationConversionAgreesWithBox3D)
{
    // A quaternion that only agrees under a matching component order will rotate a vector the same way.
    const FQuat Rotation = Math::Normalize(FQuat(FVector3(0.3f, -0.7f, 1.1f)));
    const FVector3 Vector(0.4f, 1.3f, -2.0f);

    const FVector3 Engine = Math::Rotate(Rotation, Vector);
    const FVector3 Native = Box3DUtils::FromB3Vec3(b3RotateVector(Box3DUtils::ToB3Quat(Rotation), Box3DUtils::ToB3Vec3(Vector)));

    EXPECT_NEAR(Native.x, Engine.x, 1e-4f);
    EXPECT_NEAR(Native.y, Engine.y, 1e-4f);
    EXPECT_NEAR(Native.z, Engine.z, 1e-4f);
}

TEST(Box3DPhysics, CollisionProfilePacksIntoNativeFilter)
{
    SCollisionProfile Profile;
    Profile.Layer = ECollisionProfiles::Dynamic;
    Profile.Mask = ECollisionProfiles::Static | ECollisionProfiles::Dynamic;

    const b3Filter Filter = Box3DUtils::MakeShapeFilter(Profile);
    EXPECT_EQ(Filter.categoryBits, (uint64)ECollisionProfiles::Dynamic);

    // The permissive rule widens the broad-phase mask so the pair callback gets to decide.
    if (Box3DUtils::UsesPermissiveCollisionFilter())
    {
        EXPECT_EQ(Filter.maskBits, (uint64)B3_DEFAULT_MASK_BITS);
    }
    else
    {
        EXPECT_EQ(Filter.maskBits, (uint64)(ECollisionProfiles::Static | ECollisionProfiles::Dynamic));
    }
}

TEST(Box3DPhysics, BodyTypeMappingRoundTrips)
{
    for (EBodyType Type : { EBodyType::Static, EBodyType::Kinematic, EBodyType::Dynamic })
    {
        EXPECT_EQ(Box3DUtils::FromBox3DBodyType(Box3DUtils::ToBox3DBodyType(Type)), Type);
    }
}

TEST(Box3DPhysics, BodyFallsAndRestsOnGround)
{
    FTestWorld World;
    ASSERT_TRUE(b3World_IsValid(World.WorldId));

    b3HullData* GroundHull = MakeBoxHull(50.0f, 1.0f, 50.0f);
    ASSERT_NE(GroundHull, nullptr);

    AddGround(World.WorldId, GroundHull);
    const b3BodyId Sphere = AddFallingSphere(World.WorldId, 6.0f, 0.5f);

    EXPECT_GT(b3Body_GetMass(Sphere), 0.0f);

    for (int32 i = 0; i < 240; ++i)
    {
        b3World_Step(World.WorldId, 1.0f / 60.0f, 4);
    }

    const float RestHeight = b3Body_GetPosition(Sphere).y;
    EXPECT_NEAR(RestHeight, 1.5f, 0.1f);

    b3DestroyHull(GroundHull);
}

TEST(Box3DPhysics, TaskBridgeMatchesSingleThreadedResult)
{
    // A mismatch here means the bridge is dropping or double-running work rather than just reordering it.
    b3HullData* GroundHull = MakeBoxHull(50.0f, 1.0f, 50.0f);
    ASSERT_NE(GroundHull, nullptr);

    auto RunStack = [&](bool bMultithreaded)
    {
        FTestWorld World(bMultithreaded);
        AddGround(World.WorldId, GroundHull);

        TVector<b3BodyId> Bodies;
        for (int32 i = 0; i < 48; ++i)
        {
            Bodies.Add(AddFallingSphere(World.WorldId, 2.0f + (float)i * 1.05f, 0.5f));
        }

        for (int32 i = 0; i < 180; ++i)
        {
            b3World_Step(World.WorldId, 1.0f / 60.0f, 4);
        }

        float Total = 0.0f;
        for (b3BodyId Body : Bodies)
        {
            Total += b3Body_GetPosition(Body).y;
        }
        return Total;
    };

    const float Threaded = RunStack(true);
    const float Serial = RunStack(false);

    EXPECT_GT(Threaded, 0.0f);
    EXPECT_NEAR(Threaded, Serial, Math::Abs(Serial) * 0.05f + 1.0f);

    b3DestroyHull(GroundHull);
}

TEST(Box3DPhysics, RayCastHitsBodyAndReportsNormal)
{
    FTestWorld World;
    b3HullData* GroundHull = MakeBoxHull(50.0f, 1.0f, 50.0f);
    ASSERT_NE(GroundHull, nullptr);

    AddGround(World.WorldId, GroundHull);
    b3World_Step(World.WorldId, 1.0f / 60.0f, 4);

    const b3RayResult Hit = b3World_CastRayClosest(World.WorldId, b3Vec3{ 0.0f, 5.0f, 0.0f },
                                                   b3Vec3{ 0.0f, -10.0f, 0.0f }, b3DefaultQueryFilter());
    ASSERT_TRUE(Hit.hit);
    EXPECT_NEAR(Hit.point.y, 1.0f, 0.05f);
    EXPECT_NEAR(Hit.normal.y, 1.0f, 1e-3f);

    b3DestroyHull(GroundHull);
}

TEST(Box3DPhysics, SleepingBodyStopsReportingMoveEvents)
{
    // The interpolation path is driven entirely by move events, so a resting body must stop producing them.
    FTestWorld World;
    b3HullData* GroundHull = MakeBoxHull(50.0f, 1.0f, 50.0f);
    ASSERT_NE(GroundHull, nullptr);

    AddGround(World.WorldId, GroundHull);
    const b3BodyId Sphere = AddFallingSphere(World.WorldId, 3.0f, 0.5f);

    bool bSawSleep = false;
    for (int32 i = 0; i < 600 && !bSawSleep; ++i)
    {
        b3World_Step(World.WorldId, 1.0f / 60.0f, 4);

        const b3BodyEvents Events = b3World_GetBodyEvents(World.WorldId);
        for (int32 e = 0; e < Events.moveCount; ++e)
        {
            bSawSleep |= Events.moveEvents[e].fellAsleep;
        }
    }

    EXPECT_TRUE(bSawSleep);
    EXPECT_FALSE(b3Body_IsAwake(Sphere));

    b3World_Step(World.WorldId, 1.0f / 60.0f, 4);
    EXPECT_EQ(b3World_GetBodyEvents(World.WorldId).moveCount, 0);

    b3DestroyHull(GroundHull);
}

TEST(Box3DPhysics, HullGeneratorReducesPointCloud)
{
    TVector<FVector3> Points;
    for (int32 i = 0; i < 8; ++i)
    {
        Points.Emplace((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
    }
    // An interior point must not survive the reduction.
    Points.Emplace(0.0f, 0.0f, 0.0f);

    TVector<FVector3> Hull;
    ASSERT_TRUE(Physics::CollisionGen::BuildHullPoints(Points, Hull));
    EXPECT_EQ(Hull.Num(), 8);

    TVector<FVector3> Vertices;
    TVector<uint32> Edges;
    ASSERT_TRUE(Physics::CollisionGen::BuildHullWireframe(Points, Vertices, Edges));
    EXPECT_EQ(Edges.Num(), 24);

    TVector<uint32> Indices;
    ASSERT_TRUE(Physics::CollisionGen::BuildHullTriangles(Points, Vertices, Indices));
    EXPECT_EQ(Indices.Num(), 36);
}

TEST(Box3DPhysics, FittedCapsuleEnclosesPointsAlongTheLongestAxis)
{
    // Box corners plus face centers, long on one axis, so the cylinder and both caps are exercised.
    const auto MakeBlock = [](const FVector3& Half)
    {
        TVector<FVector3> Points;
        for (int32 i = 0; i < 8; ++i)
        {
            Points.Add(FVector3((i & 1) ? Half.x : -Half.x, (i & 2) ? Half.y : -Half.y, (i & 4) ? Half.z : -Half.z) + FVector3(3.0f, -1.0f, 2.0f));
        }
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            FVector3 Face(0.0f);
            Face[Axis] = Half[Axis];
            Points.Add(Face + FVector3(3.0f, -1.0f, 2.0f));
            Points.Add(-Face + FVector3(3.0f, -1.0f, 2.0f));
        }
        return Points;
    };

    const FVector3 Halves[] = { FVector3(2.0f, 0.3f, 0.2f), FVector3(0.25f, 1.5f, 0.3f), FVector3(0.2f, 0.3f, 2.5f) };
    for (int32 LongAxis = 0; LongAxis < 3; ++LongAxis)
    {
        const TVector<FVector3> Points = MakeBlock(Halves[LongAxis]);

        SCollisionPrimitive Capsule;
        ASSERT_TRUE(Physics::CollisionGen::FitCapsule(Points, Capsule));
        EXPECT_EQ(Capsule.Type, ECollisionPrimitiveType::Capsule);

        // Rotation is authored in degrees, the way the editor draws it and the physics build reads it.
        const FVector3 CapsuleAxis = Math::Normalize(FQuat(Math::Radians(Capsule.Rotation)) * FVector3(0.0f, 1.0f, 0.0f));
        EXPECT_NEAR(Math::Abs(CapsuleAxis[LongAxis]), 1.0f, 1e-4f) << "long axis " << LongAxis;

        for (const FVector3& P : Points)
        {
            const float Along = Math::Clamp(Math::Dot(P - Capsule.Center, CapsuleAxis), -Capsule.HalfHeight, Capsule.HalfHeight);
            const float Distance = Math::Length(P - (Capsule.Center + CapsuleAxis * Along));
            EXPECT_LE(Distance, Capsule.Radius + 1e-4f) << "long axis " << LongAxis;
        }
    }

    SCollisionPrimitive Empty;
    EXPECT_FALSE(Physics::CollisionGen::FitCapsule(TVector<FVector3>(), Empty));
}

TEST(Box3DPhysics, DegenerateHullInputIsRejected)
{
    TVector<FVector3> Points{ FVector3(0.0f), FVector3(1.0f, 0.0f, 0.0f), FVector3(0.0f, 1.0f, 0.0f) };

    TVector<FVector3> Hull;
    EXPECT_FALSE(Physics::CollisionGen::BuildHullPoints(Points, Hull));
    EXPECT_TRUE(Hull.IsEmpty());
}

namespace
{
    // Mirrors the shipped mover sequence in Box3DCharacter.cpp so a failure here localizes the bug.
    struct FMoverProbe
    {
        static constexpr int32 Capacity = 16;

        b3CollisionPlane    Planes[Capacity];
        b3Vec3              Points[Capacity];
        b3ShapeId           Shapes[Capacity];
        int32               Count = 0;
        b3Vec3              Origin{};
        b3BodyId            IgnoreBody{};
        float               CosMaxSlope = 0.0f;
        bool                bUprightSteepFaces = true;
    };

    bool ProbeGatherPlanes(b3ShapeId ShapeId, const b3PlaneResult* Results, int32 PlaneCount, void* Context)
    {
        FMoverProbe& Out = *static_cast<FMoverProbe*>(Context);
        if (B3_ID_EQUALS(b3Shape_GetBody(ShapeId), Out.IgnoreBody))
        {
            return true;
        }

        for (int32 i = 0; i < PlaneCount && Out.Count < FMoverProbe::Capacity; ++i)
        {
            b3Plane Plane = Results[i].plane;

            // A face too steep to walk collides as a vertical wall, matching Box3DCharacter.cpp.
            if (Out.bUprightSteepFaces && Plane.normal.y > 0.02f && Plane.normal.y < Out.CosMaxSlope)
            {
                const float Horizontal = b3Length(b3Vec3{ Plane.normal.x, 0.0f, Plane.normal.z });
                Plane = b3Plane{ b3MulSV(1.0f / Horizontal, b3Vec3{ Plane.normal.x, 0.0f, Plane.normal.z }), Plane.offset / Horizontal };
            }

            Out.Planes[Out.Count] = b3CollisionPlane{ Plane, FLT_MAX, 0.0f, true };
            Out.Points[Out.Count] = b3Add(Out.Origin, Results[i].point);
            Out.Shapes[Out.Count] = ShapeId;
            ++Out.Count;
        }
        return true;
    }

    bool ProbeCastFilter(b3ShapeId ShapeId, void* Context)
    {
        const FMoverProbe& Probe = *static_cast<const FMoverProbe*>(Context);
        return !B3_ID_EQUALS(b3Shape_GetBody(ShapeId), Probe.IgnoreBody);
    }

    struct FMoverState
    {
        b3Vec3  Position{};
        b3Vec3  Velocity{};
        b3Vec3  GroundNormal{ 0.0f, 1.0f, 0.0f };
        bool    bGrounded = false;
    };

    // The documented order is cast, move, gather at the new pose, solve the remainder, apply and clip.
    void StepMover(b3WorldId WorldId, FMoverState& State, const b3Capsule& Mover, float Dt, float CosMaxSlope,
                   bool bUprightSteepFaces = true)
    {
        constexpr float Gravity = -9.81f;

        if (State.bGrounded)
        {
            State.Velocity.y = 0.0f;
        }
        else
        {
            State.Velocity.y += Gravity * Dt;
        }

        FMoverProbe Probe;
        Probe.CosMaxSlope = CosMaxSlope;
        Probe.bUprightSteepFaces = bUprightSteepFaces;

        b3Vec3 Desired = b3MulSV(Dt, State.Velocity);

        // Walkable floors are followed rather than projected onto, matching Box3DCharacter.cpp.
        if (State.bGrounded && State.GroundNormal.y >= CosMaxSlope && State.GroundNormal.y > 0.1f)
        {
            Desired.y = -(Desired.x * State.GroundNormal.x + Desired.z * State.GroundNormal.z) / State.GroundNormal.y;
        }

        const float TravelFraction = b3World_CastMover(WorldId, State.Position, &Mover, Desired,
                                                       b3DefaultQueryFilter(), &ProbeCastFilter, &Probe);

        State.Position = b3Add(State.Position, b3MulSV(TravelFraction, Desired));

        Probe.Count = 0;
        Probe.Origin = State.Position;
        b3World_CollideMover(WorldId, State.Position, &Mover, b3DefaultQueryFilter(), &ProbeGatherPlanes, &Probe);

        const b3Vec3 Remaining = b3MulSV(1.0f - TravelFraction, Desired);
        const b3PlaneSolverResult Solved = b3SolvePlanes(Remaining, Probe.Planes, Probe.Count);
        State.Position = b3Add(State.Position, Solved.delta);

        // Overlap recovery, matching Box3DCharacter.cpp.
        for (int32 Recovery = 0; Recovery < 8; ++Recovery)
        {
            Probe.Count = 0;
            Probe.Origin = State.Position;
            b3World_CollideMover(WorldId, State.Position, &Mover, b3DefaultQueryFilter(), &ProbeGatherPlanes, &Probe);

            const b3PlaneSolverResult Push = b3SolvePlanes(b3Vec3_zero, Probe.Planes, Probe.Count);
            if (b3LengthSquared(Push.delta) < 0.01f * 0.01f)
            {
                break;
            }

            State.Position = b3Add(State.Position, Push.delta);
        }

        Probe.Count = 0;
        Probe.Origin = State.Position;
        b3World_CollideMover(WorldId, State.Position, &Mover, b3DefaultQueryFilter(), &ProbeGatherPlanes, &Probe);

        State.bGrounded = false;
        for (int32 i = 0; i < Probe.Count; ++i)
        {
            if (Probe.Planes[i].plane.normal.y >= CosMaxSlope)
            {
                if (!State.bGrounded)
                {
                    State.GroundNormal = Probe.Planes[i].plane.normal;
                }
                State.bGrounded = true;

                // A walkable floor never takes horizontal speed, matching Box3DCharacter.cpp.
                Probe.Planes[i].clipVelocity = false;
            }
        }

        b3SolvePlanes(b3Vec3_zero, Probe.Planes, Probe.Count);
        State.Velocity = b3ClipVector(State.Velocity, Probe.Planes, Probe.Count);
    }
}

TEST(Box3DCharacter, MoverLandsOnGroundAndStaysThere)
{
    FTestWorld World;
    b3HullData* GroundHull = MakeBoxHull(50.0f, 1.0f, 50.0f);
    ASSERT_NE(GroundHull, nullptr);
    AddGround(World.WorldId, GroundHull);

    b3World_Step(World.WorldId, 1.0f / 60.0f, 4);

    constexpr float Radius = 0.5f;
    constexpr float HalfHeight = 0.9f;
    const b3Capsule Mover{ b3Vec3{ 0.0f, -HalfHeight, 0.0f }, b3Vec3{ 0.0f, HalfHeight, 0.0f }, Radius };

    FMoverState State;
    State.Position = b3Vec3{ 0.0f, 5.0f, 0.0f };

    const float CosMaxSlope = Math::Cos(Math::Radians(45.0f));

    for (int32 i = 0; i < 300; ++i)
    {
        StepMover(World.WorldId, State, Mover, 1.0f / 60.0f, CosMaxSlope);
    }

    // Ground top is y=1, so a resting capsule center sits at 1 + HalfHeight + Radius.
    const float ExpectedRest = 1.0f + HalfHeight + Radius;
    EXPECT_NEAR(State.Position.y, ExpectedRest, 0.05f) << "mover did not come to rest on the ground";
    EXPECT_TRUE(State.bGrounded);

    b3DestroyHull(GroundHull);
}

TEST(Box3DCharacter, MoverDoesNotCreepOnSlope)
{
    FTestWorld World;

    // A 20 degree ramp, well inside the 45 degree walkable limit.
    b3HullData* RampHull = MakeBoxHull(20.0f, 1.0f, 20.0f);
    ASSERT_NE(RampHull, nullptr);

    b3BodyDef BodyDef = b3DefaultBodyDef();
    BodyDef.type = b3_staticBody;
    BodyDef.position = b3Vec3{ 0.0f, 0.0f, 0.0f };

    const float Angle = Math::Radians(20.0f);
    BodyDef.rotation = b3Quat{ b3Vec3{ 0.0f, 0.0f, Math::Sin(Angle * 0.5f) }, Math::Cos(Angle * 0.5f) };

    const b3BodyId Ramp = b3CreateBody(World.WorldId, &BodyDef);
    b3ShapeDef ShapeDef = b3DefaultShapeDef();
    b3CreateHullShape(Ramp, &ShapeDef, RampHull);

    b3World_Step(World.WorldId, 1.0f / 60.0f, 4);

    constexpr float Radius = 0.5f;
    constexpr float HalfHeight = 0.9f;
    const b3Capsule Mover{ b3Vec3{ 0.0f, -HalfHeight, 0.0f }, b3Vec3{ 0.0f, HalfHeight, 0.0f }, Radius };

    FMoverState State;
    State.Position = b3Vec3{ 0.0f, 4.0f, 0.0f };

    const float CosMaxSlope = Math::Cos(Math::Radians(45.0f));

    for (int32 i = 0; i < 180; ++i)
    {
        StepMover(World.WorldId, State, Mover, 1.0f / 60.0f, CosMaxSlope);
    }

    ASSERT_TRUE(State.bGrounded) << "mover never settled on the ramp";

    const b3Vec3 Settled = State.Position;

    for (int32 i = 0; i < 300; ++i)
    {
        StepMover(World.WorldId, State, Mover, 1.0f / 60.0f, CosMaxSlope);
    }

    const float DriftX = Math::Abs(State.Position.x - Settled.x);
    const float DriftZ = Math::Abs(State.Position.z - Settled.z);

    EXPECT_LT(DriftX, 0.02f) << "mover crept along the slope over 5 seconds";
    EXPECT_LT(DriftZ, 0.02f) << "mover crept along the slope over 5 seconds";

    b3DestroyHull(RampHull);
}

// Drives a diagonal heading across the fall line, where a plane projection bends hardest.
TEST(Box3DCharacter, MoverKeepsHeadingAndSpeedRunningUpASlope)
{
    FTestWorld World;

    b3HullData* RampHull = MakeBoxHull(20.0f, 1.0f, 20.0f);
    ASSERT_NE(RampHull, nullptr);

    b3BodyDef BodyDef = b3DefaultBodyDef();
    BodyDef.type = b3_staticBody;
    BodyDef.position = b3Vec3{ 0.0f, 0.0f, 0.0f };

    const float Angle = Math::Radians(20.0f);
    BodyDef.rotation = b3Quat{ b3Vec3{ 0.0f, 0.0f, Math::Sin(Angle * 0.5f) }, Math::Cos(Angle * 0.5f) };

    const b3BodyId Ramp = b3CreateBody(World.WorldId, &BodyDef);
    b3ShapeDef ShapeDef = b3DefaultShapeDef();
    b3CreateHullShape(Ramp, &ShapeDef, RampHull);

    b3World_Step(World.WorldId, 1.0f / 60.0f, 4);

    constexpr float Radius = 0.5f;
    constexpr float HalfHeight = 0.9f;
    const b3Capsule Mover{ b3Vec3{ 0.0f, -HalfHeight, 0.0f }, b3Vec3{ 0.0f, HalfHeight, 0.0f }, Radius };

    const float CosMaxSlope = Math::Cos(Math::Radians(45.0f));

    FMoverState State;
    State.Position = b3Vec3{ 0.0f, 4.0f, 0.0f };

    for (int32 i = 0; i < 180; ++i)
    {
        StepMover(World.WorldId, State, Mover, 1.0f / 60.0f, CosMaxSlope);
    }

    ASSERT_TRUE(State.bGrounded) << "mover never settled on the ramp";

    // The ramp normal tilts toward -X, so +X is uphill and a +X/+Z heading crosses the fall line.
    constexpr float Speed = 5.0f;
    const float Component = Speed / Math::Sqrt(2.0f);
    State.Velocity = b3Vec3{ Component, 0.0f, Component };

    const b3Vec3 Start = State.Position;

    for (int32 i = 0; i < 30; ++i)
    {
        StepMover(World.WorldId, State, Mover, 1.0f / 60.0f, CosMaxSlope);
    }

    const float TravelX = State.Position.x - Start.x;
    const float TravelZ = State.Position.z - Start.z;
    const float Travel  = Math::Sqrt(TravelX * TravelX + TravelZ * TravelZ);

    ASSERT_GT(Travel, 1.0f) << "mover barely moved, so the heading check would be meaningless";

    // Airborne would preserve heading and speed trivially, which is not what this is measuring.
    ASSERT_TRUE(State.bGrounded) << "mover left the ramp, so nothing here is about slope handling";

    // The authored heading is 45 degrees between +X and +Z; a projection onto the ramp rotates it.
    const float HeadingDot = ((TravelX * Component) + (TravelZ * Component)) / (Travel * Speed);
    EXPECT_GT(HeadingDot, 0.999f) << "running up the slope bent the move off the input direction";

    const float HorizontalSpeed = Travel / (30.0f / 60.0f);
    EXPECT_NEAR(HorizontalSpeed, Speed, 0.25f) << "the slope changed horizontal speed";

    const float CarriedSpeed = Math::Sqrt(State.Velocity.x * State.Velocity.x + State.Velocity.z * State.Velocity.z);
    EXPECT_NEAR(CarriedSpeed, Speed, 0.25f) << "the slope clipped horizontal velocity away";

    b3DestroyHull(RampHull);
}

// Pushing into a face steeper than the walk limit slid the move up it, which colliding with it as a wall stops.
TEST(Box3DCharacter, MoverCannotWalkUpAFaceTooSteepToWalk)
{
    FTestWorld World;

    b3HullData* GroundHull = MakeBoxHull(50.0f, 1.0f, 50.0f);
    b3HullData* CliffHull  = MakeBoxHull(20.0f, 1.0f, 20.0f);
    ASSERT_NE(GroundHull, nullptr);
    ASSERT_NE(CliffHull, nullptr);

    b3BodyDef GroundDef = b3DefaultBodyDef();
    GroundDef.type = b3_staticBody;
    b3ShapeDef ShapeDef = b3DefaultShapeDef();
    b3CreateHullShape(b3CreateBody(World.WorldId, &GroundDef), &ShapeDef, GroundHull);

    // A 60 degree face rising toward +X that meets the ground top at x = 3.
    const float Angle = Math::Radians(60.0f);
    const b3Vec3 FaceNormal{ -Math::Sin(Angle), Math::Cos(Angle), 0.0f };
    b3BodyDef CliffDef = b3DefaultBodyDef();
    CliffDef.type = b3_staticBody;
    CliffDef.position = b3Vec3{ 3.0f - FaceNormal.x, 1.0f - FaceNormal.y, 0.0f };
    CliffDef.rotation = b3Quat{ b3Vec3{ 0.0f, 0.0f, Math::Sin(Angle * 0.5f) }, Math::Cos(Angle * 0.5f) };
    b3CreateHullShape(b3CreateBody(World.WorldId, &CliffDef), &ShapeDef, CliffHull);

    b3World_Step(World.WorldId, 1.0f / 60.0f, 4);

    constexpr float Radius = 0.5f;
    constexpr float HalfHeight = 0.9f;
    const b3Capsule Mover{ b3Vec3{ 0.0f, -HalfHeight, 0.0f }, b3Vec3{ 0.0f, HalfHeight, 0.0f }, Radius };
    const float CosMaxSlope = Math::Cos(Math::Radians(45.0f));

    auto Climb = [&](bool bUprightSteepFaces)
    {
        FMoverState State;
        State.Position = b3Vec3{ 0.0f, 1.0f + HalfHeight + Radius + 0.05f, 0.0f };
        for (int32 i = 0; i < 60; ++i)
        {
            StepMover(World.WorldId, State, Mover, 1.0f / 60.0f, CosMaxSlope, bUprightSteepFaces);
        }
        const float StartY = State.Position.y;

        // Input sets the horizontal velocity every step, the way the movement component does.
        float HighestY = StartY;
        for (int32 i = 0; i < 240; ++i)
        {
            State.Velocity.x = 6.0f;
            State.Velocity.z = 0.0f;
            StepMover(World.WorldId, State, Mover, 1.0f / 60.0f, CosMaxSlope, bUprightSteepFaces);
            HighestY = Math::Max(HighestY, State.Position.y);
        }
        return HighestY - StartY;
    };

    EXPECT_GT(Climb(false), 1.0f) << "colliding with the true face should climb, or this scene does not reproduce the bug";
    EXPECT_LT(Climb(true), 0.3f) << "the mover climbed a face steeper than its walk limit";

    // A mover dropped onto the face slides down it rather than perching where it landed.
    FMoverState Dropped;
    Dropped.Position = b3Vec3{ 5.0f, 6.5f, 0.0f };
    for (int32 i = 0; i < 240; ++i)
    {
        StepMover(World.WorldId, Dropped, Mover, 1.0f / 60.0f, CosMaxSlope);
    }
    EXPECT_LT(Dropped.Position.y, 3.0f) << "a mover that landed on the face stayed up on it";
    EXPECT_TRUE(Dropped.bGrounded) << "the mover should come to rest on the ground below the face";

    b3DestroyHull(GroundHull);
    b3DestroyHull(CliffHull);
}

TEST(Box3DCharacter, MoverQueryFilterAcceptsTypicalGroundProfiles)
{
    SCollisionProfile Character;
    Character.Layer = ECollisionProfiles::Dynamic;
    Character.Mask = ECollisionProfiles::Static | ECollisionProfiles::Dynamic;

    const b3QueryFilter Query = Box3DUtils::MakeQueryFilter(Character);

    auto Accepts = [&](ECollisionProfiles GroundLayer, ECollisionProfiles GroundMask)
    {
        SCollisionProfile Ground;
        Ground.Layer = GroundLayer;
        Ground.Mask = GroundMask;

        const b3Filter Shape = Box3DUtils::MakeShapeFilter(Ground);
        const bool bBroadPhase = (Shape.categoryBits & Query.maskBits) != 0 && (Shape.maskBits & Query.categoryBits) != 0;
        return bBroadPhase && Ground.ShouldCollide(Character);
    };

    EXPECT_TRUE(Accepts(ECollisionProfiles::Static, ECollisionProfiles::Static | ECollisionProfiles::Dynamic));
    EXPECT_TRUE(Accepts(ECollisionProfiles::Dynamic, ECollisionProfiles::Static | ECollisionProfiles::Dynamic));

    // Ground that only lists other static geometry in its mask must still support the character.
    EXPECT_TRUE(Accepts(ECollisionProfiles::Static, ECollisionProfiles::Static));
}

TEST(Box3DPhysics, HeightFieldIsIndexedFromTheBodyOrigin)
{
    // A height field grid starts at the body origin, so the engine folds terrain's half-tile offset into the body.
    FTestWorld World;

    constexpr int32 Resolution = 9;
    constexpr float TileWorldSize = 80.0f;
    constexpr float FlatHeight = 3.0f;

    TVector<float> Heights;
    Heights.SetNum(Lumina::IntCastChecked<Lumina::int32>(static_cast<int64>(Resolution) * Resolution), FlatHeight);

    const float Stride = TileWorldSize / float(Resolution - 1);

    b3HeightFieldDef Def{};
    Def.heights = Heights.GetData();
    Def.countX = Resolution;
    Def.countZ = Resolution;
    Def.scale = b3Vec3{ Stride, 1.0f, Stride };
    Def.globalMinimumHeight = FlatHeight - 1.0f;
    Def.globalMaximumHeight = FlatHeight + 1.0f;

    b3HeightFieldData* Field = b3CreateHeightField(&Def);
    ASSERT_NE(Field, nullptr);

    // The entity sits at the origin, so the body carries the shift the shape cannot express.
    b3BodyDef BodyDef = b3DefaultBodyDef();
    BodyDef.type = b3_staticBody;
    BodyDef.position = b3Vec3{ -TileWorldSize * 0.5f, 0.0f, -TileWorldSize * 0.5f };

    const b3BodyId Body = b3CreateBody(World.WorldId, &BodyDef);
    b3ShapeDef ShapeDef = b3DefaultShapeDef();
    b3CreateHeightFieldShape(Body, &ShapeDef, Field);

    b3World_Step(World.WorldId, 1.0f / 60.0f, 4);

    // Straight down through the tile center, which is where the entity is.
    const b3RayResult Center = b3World_CastRayClosest(World.WorldId, b3Vec3{ 0.0f, 20.0f, 0.0f },
                                                      b3Vec3{ 0.0f, -40.0f, 0.0f }, b3DefaultQueryFilter());
    EXPECT_TRUE(Center.hit) << "no terrain under the tile center";
    EXPECT_NEAR(Center.point.y, FlatHeight, 0.1f);

    // A point well inside the tile but away from the center must also be covered.
    const b3RayResult Offset = b3World_CastRayClosest(World.WorldId, b3Vec3{ 25.0f, 20.0f, -25.0f },
                                                      b3Vec3{ 0.0f, -40.0f, 0.0f }, b3DefaultQueryFilter());
    EXPECT_TRUE(Offset.hit) << "terrain does not cover the whole tile, so it is misplaced";
    EXPECT_NEAR(Offset.point.y, FlatHeight, 0.1f);

    b3DestroyHeightField(Field);
}

TEST(Box3DCharacter, MoverLandsOnTriangleMeshGround)
{
    // A dynamic mesh collider is a triangle surface with no interior, so sinking past it once is unrecoverable.
    FTestWorld World;

    b3MeshData* Grid = b3CreateGridMesh(40, 40, 2.0f, 1, true);
    ASSERT_NE(Grid, nullptr);

    b3BodyDef BodyDef = b3DefaultBodyDef();
    BodyDef.type = b3_staticBody;
    BodyDef.position = b3Vec3{ 0.0f, 0.0f, 0.0f };

    const b3BodyId Body = b3CreateBody(World.WorldId, &BodyDef);
    b3ShapeDef ShapeDef = StaticGroundShapeDef();
    b3CreateMeshShape(Body, &ShapeDef, Grid, b3Vec3{ 1.0f, 1.0f, 1.0f });

    b3World_Step(World.WorldId, 1.0f / 60.0f, 4);

    const b3RayResult Probe = b3World_CastRayClosest(World.WorldId, b3Vec3{ 0.0f, 10.0f, 0.0f },
                                                     b3Vec3{ 0.0f, -20.0f, 0.0f }, b3DefaultQueryFilter());
    ASSERT_TRUE(Probe.hit) << "the grid mesh is not where the test assumes";
    const float SurfaceY = Probe.point.y;

    constexpr float Radius = 0.5f;
    constexpr float HalfHeight = 0.9f;
    const b3Capsule Mover{ b3Vec3{ 0.0f, -HalfHeight, 0.0f }, b3Vec3{ 0.0f, HalfHeight, 0.0f }, Radius };

    FMoverState State;
    State.Position = b3Vec3{ 0.0f, SurfaceY + 6.0f, 0.0f };

    const float CosMaxSlope = Math::Cos(Math::Radians(45.0f));

    for (int32 i = 0; i < 300; ++i)
    {
        StepMover(World.WorldId, State, Mover, 1.0f / 60.0f, CosMaxSlope);
    }

    EXPECT_NEAR(State.Position.y, SurfaceY + HalfHeight + Radius, 0.1f) << "mover fell through the mesh";
    EXPECT_TRUE(State.bGrounded);

    b3DestroyMesh(Grid);
}

TEST(Box3DCharacter, SpawnInsideMeshGroundIsSeatedOnTheSurface)
{
    // A spawner places the entity origin and the capsule is centered on it, so a ground-level spawn is buried.
    FTestWorld World;

    b3MeshData* Grid = b3CreateGridMesh(40, 40, 2.0f, 1, true);
    ASSERT_NE(Grid, nullptr);

    b3BodyDef BodyDef = b3DefaultBodyDef();
    BodyDef.type = b3_staticBody;
    BodyDef.position = b3Vec3{ 0.0f, 0.0f, 0.0f };

    const b3BodyId Body = b3CreateBody(World.WorldId, &BodyDef);
    b3ShapeDef ShapeDef = StaticGroundShapeDef();
    b3CreateMeshShape(Body, &ShapeDef, Grid, b3Vec3{ 1.0f, 1.0f, 1.0f });

    b3World_Step(World.WorldId, 1.0f / 60.0f, 4);

    const b3RayResult Probe = b3World_CastRayClosest(World.WorldId, b3Vec3{ 0.0f, 10.0f, 0.0f },
                                                     b3Vec3{ 0.0f, -20.0f, 0.0f }, b3DefaultQueryFilter());
    ASSERT_TRUE(Probe.hit);
    const float SurfaceY = Probe.point.y;

    constexpr float Radius = 1.0f;
    constexpr float HalfHeight = 1.8f;
    const b3Capsule Mover{ b3Vec3{ 0.0f, -HalfHeight, 0.0f }, b3Vec3{ 0.0f, HalfHeight, 0.0f }, Radius };

    const b3Vec3 Buried{ 0.0f, SurfaceY, 0.0f };
    b3Vec3 Seated;
    const Physics::EMoverSeatResult Result = Physics::TrySeatMoverOnGround(World.WorldId, Mover, Buried,
                                                                          b3DefaultQueryFilter(), SCollisionProfile{}, 0.5f, b3_nullBodyId, Seated);

    EXPECT_EQ(Result, Physics::EMoverSeatResult::Seated);
    EXPECT_NEAR(Seated.y, SurfaceY + HalfHeight + Radius, 0.01f) << "spawn was not seated on the surface";

    // Once seated the mover holds it, rather than free falling the way a buried spawn does.
    FMoverState State;
    State.Position = Seated;

    const float CosMaxSlope = Math::Cos(Math::Radians(45.0f));
    for (int32 i = 0; i < 240; ++i)
    {
        StepMover(World.WorldId, State, Mover, 1.0f / 60.0f, CosMaxSlope);
    }

    EXPECT_NEAR(State.Position.y, SurfaceY + HalfHeight + Radius, 0.15f) << "mover did not hold the seated pose";
    EXPECT_TRUE(State.bGrounded);

    b3DestroyMesh(Grid);
}

TEST(Box3DCharacter, SeatingReportsNoGeometryBeforeTheGroundBodyExists)
{
    // A dynamic mesh collider defers until its CPU data is ready, so the character must hold rather than fall.
    FTestWorld World;

    constexpr float Radius = 1.0f;
    constexpr float HalfHeight = 1.8f;
    const b3Capsule Mover{ b3Vec3{ 0.0f, -HalfHeight, 0.0f }, b3Vec3{ 0.0f, HalfHeight, 0.0f }, Radius };

    b3Vec3 Seated;
    const Physics::EMoverSeatResult Empty = Physics::TrySeatMoverOnGround(World.WorldId, Mover,
        b3Vec3{ 0.0f, 0.0f, 0.0f }, b3DefaultQueryFilter(), SCollisionProfile{}, 0.5f, b3_nullBodyId, Seated);

    EXPECT_EQ(Empty, Physics::EMoverSeatResult::NoGeometry);

    // A deliberate spawn high above real ground must still be allowed to fall.
    b3HullData* GroundHull = MakeBoxHull(50.0f, 1.0f, 50.0f);
    ASSERT_NE(GroundHull, nullptr);
    AddGround(World.WorldId, GroundHull);
    b3World_Step(World.WorldId, 1.0f / 60.0f, 4);

    const Physics::EMoverSeatResult High = Physics::TrySeatMoverOnGround(World.WorldId, Mover,
        b3Vec3{ 0.0f, 200.0f, 0.0f }, b3DefaultQueryFilter(), SCollisionProfile{}, 0.5f, b3_nullBodyId, Seated);

    EXPECT_EQ(High, Physics::EMoverSeatResult::Airborne);

    b3DestroyHull(GroundHull);
}

