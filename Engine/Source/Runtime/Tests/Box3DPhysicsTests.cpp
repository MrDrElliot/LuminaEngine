#include <gtest/gtest.h>

#include <box3d/box3d.h>
#include <box3d/collision.h>

#include "Physics/API/Box3D/Box3DTaskBridge.h"
#include "Physics/API/Box3D/Box3DUtils.h"
#include "Physics/API/Box3D/Box3DInternal.h"
#include "Physics/CollisionShapeGen.h"

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
        FCollisionProfile Ground;
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
    FCollisionProfile Profile;
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
            Bodies.push_back(AddFallingSphere(World.WorldId, 2.0f + (float)i * 1.05f, 0.5f));
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
        Points.push_back(FVector3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f));
    }
    // An interior point must not survive the reduction.
    Points.push_back(FVector3(0.0f, 0.0f, 0.0f));

    TVector<FVector3> Hull;
    ASSERT_TRUE(Physics::CollisionGen::BuildHullPoints(Points, Hull));
    EXPECT_EQ(Hull.size(), 8u);

    TVector<FVector3> Vertices;
    TVector<uint32> Edges;
    ASSERT_TRUE(Physics::CollisionGen::BuildHullWireframe(Points, Vertices, Edges));
    EXPECT_EQ(Edges.size(), 24u);

    TVector<uint32> Indices;
    ASSERT_TRUE(Physics::CollisionGen::BuildHullTriangles(Points, Vertices, Indices));
    EXPECT_EQ(Indices.size(), 36u);
}

TEST(Box3DPhysics, DegenerateHullInputIsRejected)
{
    TVector<FVector3> Points{ FVector3(0.0f), FVector3(1.0f, 0.0f, 0.0f), FVector3(0.0f, 1.0f, 0.0f) };

    TVector<FVector3> Hull;
    EXPECT_FALSE(Physics::CollisionGen::BuildHullPoints(Points, Hull));
    EXPECT_TRUE(Hull.empty());
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
            Out.Planes[Out.Count] = b3CollisionPlane{ Results[i].plane, FLT_MAX, 0.0f, true };
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
        bool    bGrounded = false;
    };

    // The documented order is cast, move, gather at the new pose, solve the remainder, apply and clip.
    void StepMover(b3WorldId WorldId, FMoverState& State, const b3Capsule& Mover, float Dt, float CosMaxSlope)
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

        const b3Vec3 Desired = b3MulSV(Dt, State.Velocity);
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
                State.bGrounded = true;
                break;
            }
        }

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

TEST(Box3DCharacter, MoverQueryFilterAcceptsTypicalGroundProfiles)
{
    FCollisionProfile Character;
    Character.Layer = ECollisionProfiles::Dynamic;
    Character.Mask = ECollisionProfiles::Static | ECollisionProfiles::Dynamic;

    const b3QueryFilter Query = Box3DUtils::MakeQueryFilter(Character);

    auto Accepts = [&](ECollisionProfiles GroundLayer, ECollisionProfiles GroundMask)
    {
        FCollisionProfile Ground;
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
    Heights.resize((size_t)Resolution * (size_t)Resolution, FlatHeight);

    const float Stride = TileWorldSize / float(Resolution - 1);

    b3HeightFieldDef Def{};
    Def.heights = Heights.data();
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
                                                                          b3DefaultQueryFilter(), FCollisionProfile{}, 0.5f, b3_nullBodyId, Seated);

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
        b3Vec3{ 0.0f, 0.0f, 0.0f }, b3DefaultQueryFilter(), FCollisionProfile{}, 0.5f, b3_nullBodyId, Seated);

    EXPECT_EQ(Empty, Physics::EMoverSeatResult::NoGeometry);

    // A deliberate spawn high above real ground must still be allowed to fall.
    b3HullData* GroundHull = MakeBoxHull(50.0f, 1.0f, 50.0f);
    ASSERT_NE(GroundHull, nullptr);
    AddGround(World.WorldId, GroundHull);
    b3World_Step(World.WorldId, 1.0f / 60.0f, 4);

    const Physics::EMoverSeatResult High = Physics::TrySeatMoverOnGround(World.WorldId, Mover,
        b3Vec3{ 0.0f, 200.0f, 0.0f }, b3DefaultQueryFilter(), FCollisionProfile{}, 0.5f, b3_nullBodyId, Seated);

    EXPECT_EQ(High, Physics::EMoverSeatResult::Airborne);

    b3DestroyHull(GroundHull);
}

