#include "RuntimePCH.h"
#include "Box3DPhysics.h"

#include <box3d/collision.h>

#include "Box3DPhysicsScene.h"
#include "Box3DUtils.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Profiler/Profile.h"
#include "Log/Log.h"
#include "Memory/MemoryTracking.h"
#include "Renderer/ImmediateLineRenderer.h"
#include "World/World.h"
#include "World/Entity/Systems/DebugDrawSystem.h"

#if defined(LE_PLATFORM_WINDOWS)
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent();
#else
[[maybe_unused]] static int IsDebuggerPresent() { return 0; }
#endif

namespace Lumina::Physics
{
    static TUniquePtr<FBox3DData> Box3DData;

    static TConsoleVar CVarPhysicsDebug("Physics.Debug.Draw", false, "Toggles debug drawing for Box3D physics, has severe performance impact.");
    static TConsoleVar CVarPhysicsDebugShapes("Physics.Debug.Shapes", true, "Draw collision shape wireframes.");
    static TConsoleVar CVarPhysicsDebugJoints("Physics.Debug.Joints", true, "Draw joints.");
    static TConsoleVar CVarPhysicsDebugJointExtras("Physics.Debug.JointExtras", false, "Draw joint limits and reference frames.");
    static TConsoleVar CVarPhysicsDebugAABB("Physics.Debug.AABB", false, "Draw broad-phase bounds.");
    static TConsoleVar CVarPhysicsDebugMass("Physics.Debug.Mass", false, "Draw center of mass markers.");
    static TConsoleVar CVarPhysicsDebugSleep("Physics.Debug.Sleep", false, "Tint bodies by sleep state.");
    static TConsoleVar CVarPhysicsDebugBodyNames("Physics.Debug.BodyNames", false, "Draw body names.");
    static TConsoleVar CVarPhysicsDebugContacts("Physics.Debug.Contacts", false, "Draw contact points.");
    static TConsoleVar CVarPhysicsDebugContactNormals("Physics.Debug.ContactNormals", false, "Draw contact normals.");
    static TConsoleVar CVarPhysicsDebugContactForces("Physics.Debug.ContactForces", false, "Draw contact impulses.");
    static TConsoleVar CVarPhysicsDebugContactFeatures("Physics.Debug.ContactFeatures", false, "Draw contact feature ids.");
    static TConsoleVar CVarPhysicsDebugGraphColors("Physics.Debug.GraphColors", false, "Tint bodies by constraint graph color.");
    static TConsoleVar CVarPhysicsDebugIslands("Physics.Debug.Islands", false, "Draw simulation islands.");
    static TConsoleVar CVarPhysicsDebugDrawDistance("Physics.Debug.DrawDistance", 0.0f,
        "Half extent of the box Box3D prunes its broad phase against, centered on the camera. 0 leaves it unbounded and culls per shape instead.");

    static void* Box3DAllocate(int32_t Size, int32_t Alignment)
    {
        LUMINA_MEMORY_SCOPE("Physics");
        return Memory::Malloc((size_t)Size, (size_t)Alignment);
    }

    static void Box3DFree(void* Block)
    {
        Memory::Free(Block);
    }

    static void Box3DLog(const char* Message)
    {
        if (Box3DData)
        {
            Box3DData->LastErrorMessage = Message;
        }
        LOG_TRACE("Box3D - {}", Message);
    }

    static int Box3DAssertFailed(const char* Condition, const char* FileName, int LineNumber)
    {
        LOG_CRITICAL("BOX3D ASSERT FAILED: {} at {}({})", Condition, FileName, LineNumber);

        // Returning non-zero breaks; only do that under a debugger so a standalone run logs and continues.
        return ::IsDebuggerPresent() != 0 ? 1 : 0;
    }

    namespace
    {
        // A single shape's wireframe is capped so one giant terrain collider cannot stall the frame.
        constexpr int32 kMaxDebugShapePoints = 200000;

        FORCEINLINE uint32 PackDebugColor(b3HexColor Color)
        {
            const uint32 Rgb = (uint32)Color & 0x00FFFFFFu;
            const uint32 R = (Rgb >> 16) & 0xFF;
            const uint32 G = (Rgb >> 8) & 0xFF;
            const uint32 B = Rgb & 0xFF;
            return (0xFFu << 24) | (B << 16) | (G << 8) | R;
        }

        void AppendLine(TVector<FVector3>& Out, const b3Vec3& A, const b3Vec3& B)
        {
            Out.push_back(Box3DUtils::FromB3Vec3(A));
            Out.push_back(Box3DUtils::FromB3Vec3(B));
        }

        void AppendCircle(TVector<FVector3>& Out, const FVector3& Center, const FVector3& AxisU, const FVector3& AxisV, float Radius, int32 Segments)
        {
            FVector3 Previous = Center + AxisU * Radius;
            for (int32 i = 1; i <= Segments; ++i)
            {
                const float Angle = ((2.0f * LE_PI_F) * (float)i) / (float)Segments;
                const FVector3 Point = Center + (AxisU * Math::Cos(Angle) + AxisV * Math::Sin(Angle)) * Radius;
                Out.push_back(Previous);
                Out.push_back(Point);
                Previous = Point;
            }
        }

        void AppendSphereLines(TVector<FVector3>& Out, const b3Sphere& Sphere)
        {
            const FVector3 Center = Box3DUtils::FromB3Vec3(Sphere.center);
            constexpr int32 Segments = 24;
            AppendCircle(Out, Center, FVector3(1, 0, 0), FVector3(0, 1, 0), Sphere.radius, Segments);
            AppendCircle(Out, Center, FVector3(0, 1, 0), FVector3(0, 0, 1), Sphere.radius, Segments);
            AppendCircle(Out, Center, FVector3(1, 0, 0), FVector3(0, 0, 1), Sphere.radius, Segments);
        }

        void AppendCapsuleLines(TVector<FVector3>& Out, const b3Capsule& Capsule)
        {
            const FVector3 A = Box3DUtils::FromB3Vec3(Capsule.center1);
            const FVector3 B = Box3DUtils::FromB3Vec3(Capsule.center2);
            const float Radius = Capsule.radius;

            FVector3 Axis = B - A;
            const float Length = Math::Length(Axis);
            Axis = Length > LE_SMALL_NUMBER ? Axis / Length : FVector3(0, 1, 0);

            FVector3 U = Math::Abs(Axis.y) < 0.99f ? Math::Cross(Axis, FVector3(0, 1, 0)) : Math::Cross(Axis, FVector3(1, 0, 0));
            U = Math::Normalize(U);
            const FVector3 V = Math::Cross(Axis, U);

            constexpr int32 Segments = 24;
            AppendCircle(Out, A, U, V, Radius, Segments);
            AppendCircle(Out, B, U, V, Radius, Segments);

            for (int32 i = 0; i < 4; ++i)
            {
                const float Angle = ((2.0f * LE_PI_F) * (float)i) / 4.0f;
                const FVector3 Offset = (U * Math::Cos(Angle) + V * Math::Sin(Angle)) * Radius;
                Out.push_back(A + Offset);
                Out.push_back(B + Offset);
            }

            AppendCircle(Out, A, U, -Axis, Radius, Segments);
            AppendCircle(Out, A, V, -Axis, Radius, Segments);
            AppendCircle(Out, B, U, Axis, Radius, Segments);
            AppendCircle(Out, B, V, Axis, Radius, Segments);
        }

        void AppendHullLines(TVector<FVector3>& Out, const b3HullData* Hull, const b3Transform& Transform, const b3Vec3& Scale)
        {
            if (Hull == nullptr)
            {
                return;
            }

            const b3Vec3* Points = b3GetHullPoints(Hull);
            const b3HullHalfEdge* Edges = b3GetHullEdges(Hull);
            if (Points == nullptr || Edges == nullptr)
            {
                return;
            }

            for (int32 i = 0; i < Hull->edgeCount; ++i)
            {
                // Half edges come in twinned pairs, so only the lower index of each pair emits a line.
                const uint8 Twin = Edges[i].twin;
                if ((int32)Twin < i)
                {
                    continue;
                }

                const b3Vec3 A = b3TransformPoint(Transform, b3Mul(Scale, Points[Edges[i].origin]));
                const b3Vec3 B = b3TransformPoint(Transform, b3Mul(Scale, Points[Edges[Twin].origin]));
                AppendLine(Out, A, B);
            }
        }

        void AppendMeshLines(TVector<FVector3>& Out, const b3MeshData* Mesh, const b3Transform& Transform, const b3Vec3& Scale)
        {
            if (Mesh == nullptr)
            {
                return;
            }

            const b3Vec3* Vertices = b3GetMeshVertices(Mesh);
            const b3MeshTriangle* Triangles = b3GetMeshTriangles(Mesh);
            if (Vertices == nullptr || Triangles == nullptr)
            {
                return;
            }

            for (int32 i = 0; i < Mesh->triangleCount; ++i)
            {
                if ((int32)Out.size() >= kMaxDebugShapePoints)
                {
                    break;
                }

                const b3MeshTriangle& Tri = Triangles[i];
                const b3Vec3 A = b3TransformPoint(Transform, b3Mul(Scale, Vertices[Tri.index1]));
                const b3Vec3 B = b3TransformPoint(Transform, b3Mul(Scale, Vertices[Tri.index2]));
                const b3Vec3 C = b3TransformPoint(Transform, b3Mul(Scale, Vertices[Tri.index3]));
                AppendLine(Out, A, B);
                AppendLine(Out, B, C);
                AppendLine(Out, C, A);
            }
        }

        void AppendHeightFieldLines(TVector<FVector3>& Out, const b3HeightFieldData* Field)
        {
            if (Field == nullptr)
            {
                return;
            }

            const uint16_t* Heights = b3GetHeightFieldCompressedHeights(Field);
            if (Heights == nullptr)
            {
                return;
            }

            const b3Vec3 Scale = Field->scale;
            auto SampleAt = [&](int32 Row, int32 Column)
            {
                const float Height = Field->minHeight + Field->heightScale * (float)Heights[Row * Field->columnCount + Column];
                return FVector3((float)Column * Scale.x, Height * Scale.y, (float)Row * Scale.z);
            };

            for (int32 Row = 0; Row < Field->rowCount; ++Row)
            {
                for (int32 Column = 0; Column < Field->columnCount; ++Column)
                {
                    if ((int32)Out.size() >= kMaxDebugShapePoints)
                    {
                        return;
                    }

                    const FVector3 Origin = SampleAt(Row, Column);
                    if (Column + 1 < Field->columnCount)
                    {
                        Out.push_back(Origin);
                        Out.push_back(SampleAt(Row, Column + 1));
                    }
                    if (Row + 1 < Field->rowCount)
                    {
                        Out.push_back(Origin);
                        Out.push_back(SampleAt(Row + 1, Column));
                    }
                }
            }
        }

        void AppendCompoundLines(TVector<FVector3>& Out, const b3CompoundData* Compound)
        {
            if (Compound == nullptr)
            {
                return;
            }

            constexpr b3Vec3 UnitScale{ 1.0f, 1.0f, 1.0f };

            for (int32 i = 0; i < Compound->hullCount; ++i)
            {
                const b3CompoundHull Child = b3GetCompoundHull(Compound, i);
                AppendHullLines(Out, Child.hull, Child.transform, UnitScale);
            }

            for (int32 i = 0; i < Compound->capsuleCount; ++i)
            {
                const b3CompoundCapsule Child = b3GetCompoundCapsule(Compound, i);
                AppendCapsuleLines(Out, Child.capsule);
            }

            for (int32 i = 0; i < Compound->sphereCount; ++i)
            {
                const b3CompoundSphere Child = b3GetCompoundSphere(Compound, i);
                AppendSphereLines(Out, Child.sphere);
            }

            for (int32 i = 0; i < Compound->meshCount; ++i)
            {
                const b3CompoundMesh Child = b3GetCompoundMesh(Compound, i);
                AppendMeshLines(Out, Child.meshData, Child.transform, Child.scale);
            }
        }
    }

    void* FBox3DDebugRenderer::CreateDebugShape(const b3DebugShape* Shape, void* /*UserContext*/)
    {
        LUMINA_MEMORY_SCOPE("Physics");

        FBox3DDebugShape* Result = Memory::New<FBox3DDebugShape>();
        constexpr b3Transform Identity{ { 0.0f, 0.0f, 0.0f }, { { 0.0f, 0.0f, 0.0f }, 1.0f } };
        constexpr b3Vec3 UnitScale{ 1.0f, 1.0f, 1.0f };

        switch (Shape->type)
        {
            case b3_sphereShape:   AppendSphereLines(Result->LinePoints, *Shape->sphere); break;
            case b3_capsuleShape:  AppendCapsuleLines(Result->LinePoints, *Shape->capsule); break;
            case b3_hullShape:     AppendHullLines(Result->LinePoints, Shape->hull, Identity, UnitScale); break;
            case b3_meshShape:     AppendMeshLines(Result->LinePoints, Shape->mesh->data, Identity, Shape->mesh->scale); break;
            case b3_heightShape:   AppendHeightFieldLines(Result->LinePoints, Shape->heightField); break;
            case b3_compoundShape: AppendCompoundLines(Result->LinePoints, Shape->compound); break;
            default: break;
        }

        float MaxLengthSq = 0.0f;
        for (const FVector3& Point : Result->LinePoints)
        {
            MaxLengthSq = Math::Max(MaxLengthSq, Math::Dot(Point, Point));
        }
        Result->BoundingRadius = Math::Sqrt(MaxLengthSq);

        return Result;
    }

    void FBox3DDebugRenderer::DestroyDebugShape(void* UserShape, void* /*UserContext*/)
    {
        Memory::Delete(static_cast<FBox3DDebugShape*>(UserShape));
    }

    void FBox3DDebugRenderer::Line(const FVector3& From, const FVector3& To, uint32 PackedColor)
    {
        if (Lines != nullptr)
        {
            Lines->Line(From, To, PackedColor);
            return;
        }

        const float DrawDuration = (float)Math::Max(World->GetWorldDeltaTime(), Duration);
        const FVector4 Color(((PackedColor) & 0xFF) / 255.0f, ((PackedColor >> 8) & 0xFF) / 255.0f,
                             ((PackedColor >> 16) & 0xFF) / 255.0f, ((PackedColor >> 24) & 0xFF) / 255.0f);
        World->DrawLine(From, To, Color, 1.0f, true, DrawDuration);
    }

    void FBox3DDebugRenderer::Segments(const FVector3* Points, int32 PointCount, uint32 PackedColor)
    {
        const int32 VertexCount = (PointCount / 2) * 2;
        if (VertexCount == 0)
        {
            return;
        }

        if (Lines != nullptr)
        {
            FSimpleElementVertex* Vertices = Lines->AllocLines((uint32)(VertexCount / 2));
            if (Vertices == nullptr)
            {
                return;
            }

            for (int32 i = 0; i < VertexCount; ++i)
            {
                Vertices[i].Position = Points[i];
                Vertices[i].Color    = PackedColor;
            }
            return;
        }

        for (int32 i = 0; i < VertexCount; i += 2)
        {
            Line(Points[i], Points[i + 1], PackedColor);
        }
    }

    void FBox3DDebugRenderer::Segments(const FVector3* Points, int32 PointCount, const FVector3& Translation,
                                       const FQuat& Rotation, uint32 PackedColor)
    {
        const int32 VertexCount = (PointCount / 2) * 2;
        if (VertexCount == 0)
        {
            return;
        }

        // Rotating the basis once turns the per-point quaternion rotate into three multiply-adds.
        const FVector3 AxisX = Math::Rotate(Rotation, FVector3(1.0f, 0.0f, 0.0f));
        const FVector3 AxisY = Math::Rotate(Rotation, FVector3(0.0f, 1.0f, 0.0f));
        const FVector3 AxisZ = Math::Rotate(Rotation, FVector3(0.0f, 0.0f, 1.0f));

        if (Lines != nullptr)
        {
            FSimpleElementVertex* Vertices = Lines->AllocLines((uint32)(VertexCount / 2));
            if (Vertices == nullptr)
            {
                return;
            }

            for (int32 i = 0; i < VertexCount; ++i)
            {
                const FVector3& Local = Points[i];
                Vertices[i].Position = Translation + AxisX * Local.x + AxisY * Local.y + AxisZ * Local.z;
                Vertices[i].Color    = PackedColor;
            }
            return;
        }

        for (int32 i = 0; i < VertexCount; i += 2)
        {
            const FVector3& A = Points[i];
            const FVector3& B = Points[i + 1];
            Line(Translation + AxisX * A.x + AxisY * A.y + AxisZ * A.z,
                 Translation + AxisX * B.x + AxisY * B.y + AxisZ * B.z, PackedColor);
        }
    }

    bool FBox3DDebugRenderer::ShouldDrawShape(const FVector3& Center, float Radius) const
    {
        return DrawState == nullptr || DebugDraw::ShouldDraw(*DrawState, Center, Radius);
    }

    TVector<FVector3>& FBox3DDebugRenderer::TakeScratch()
    {
        Scratch.clear();
        return Scratch;
    }

    namespace
    {
        FBox3DDebugRenderer& RendererFrom(void* Context)
        {
            return *static_cast<FBox3DDebugRenderer*>(Context);
        }

        void DebugDrawShape(void* UserShape, b3WorldTransform Transform, b3HexColor Color, void* Context)
        {
            const FBox3DDebugShape* Shape = static_cast<const FBox3DDebugShape*>(UserShape);
            if (Shape == nullptr || Shape->LinePoints.empty())
            {
                return;
            }

            FBox3DDebugRenderer& Renderer = RendererFrom(Context);
            const FVector3 Translation = Box3DUtils::FromB3Vec3(Transform.p);

            // Box3D only tested this body against an axis-aligned box, so the frustum test still prunes.
            if (!Renderer.ShouldDrawShape(Translation, Shape->BoundingRadius))
            {
                return;
            }

            Renderer.Segments(Shape->LinePoints.data(), (int32)Shape->LinePoints.size(), Translation,
                              Box3DUtils::FromB3Quat(Transform.q), PackDebugColor(Color));
        }

        void DebugDrawSegment(b3Pos P1, b3Pos P2, b3HexColor Color, void* Context)
        {
            RendererFrom(Context).Line(Box3DUtils::FromB3Vec3(P1), Box3DUtils::FromB3Vec3(P2), PackDebugColor(Color));
        }

        void DebugDrawTransform(b3WorldTransform Transform, void* Context)
        {
            FBox3DDebugRenderer& Renderer = RendererFrom(Context);
            const FVector3 Origin = Box3DUtils::FromB3Vec3(Transform.p);
            const FQuat Rotation = Box3DUtils::FromB3Quat(Transform.q);
            constexpr float AxisLength = 0.25f;

            Renderer.Line(Origin, Origin + Math::Rotate(Rotation, FVector3(AxisLength, 0, 0)), 0xFF0000FFu);
            Renderer.Line(Origin, Origin + Math::Rotate(Rotation, FVector3(0, AxisLength, 0)), 0xFF00FF00u);
            Renderer.Line(Origin, Origin + Math::Rotate(Rotation, FVector3(0, 0, AxisLength)), 0xFFFF0000u);
        }

        void DebugDrawPoint(b3Pos P, float Size, b3HexColor Color, void* Context)
        {
            const FVector3 Center = Box3DUtils::FromB3Vec3(P);
            const float Extent = Math::Max(Size, 0.01f) * 0.5f;

            const FVector3 Points[6] =
            {
                Center - FVector3(Extent, 0, 0), Center + FVector3(Extent, 0, 0),
                Center - FVector3(0, Extent, 0), Center + FVector3(0, Extent, 0),
                Center - FVector3(0, 0, Extent), Center + FVector3(0, 0, Extent),
            };

            RendererFrom(Context).Segments(Points, 6, PackDebugColor(Color));
        }

        void DebugDrawSphere(b3Pos P, float Radius, b3HexColor Color, float /*Alpha*/, void* Context)
        {
            FBox3DDebugRenderer& Renderer = RendererFrom(Context);

            TVector<FVector3>& Points = Renderer.TakeScratch();
            AppendSphereLines(Points, b3Sphere{ P, Radius });
            Renderer.Segments(Points.data(), (int32)Points.size(), PackDebugColor(Color));
        }

        void DebugDrawCapsule(b3Pos P1, b3Pos P2, float Radius, b3HexColor Color, float /*Alpha*/, void* Context)
        {
            FBox3DDebugRenderer& Renderer = RendererFrom(Context);

            TVector<FVector3>& Points = Renderer.TakeScratch();
            AppendCapsuleLines(Points, b3Capsule{ P1, P2, Radius });
            Renderer.Segments(Points.data(), (int32)Points.size(), PackDebugColor(Color));
        }

        void DrawBoxWireframe(FBox3DDebugRenderer& Renderer, const FVector3& Center, const FVector3& Extents, const FQuat& Rotation, uint32 Packed)
        {
            FVector3 Corners[8];
            for (int32 i = 0; i < 8; ++i)
            {
                const FVector3 Sign((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
                Corners[i] = Center + Math::Rotate(Rotation, Sign * Extents);
            }

            static constexpr int32 EdgeIndices[12][2] =
            {
                {0,1},{2,3},{4,5},{6,7},
                {0,2},{1,3},{4,6},{5,7},
                {0,4},{1,5},{2,6},{3,7},
            };

            FVector3 Points[24];
            for (int32 i = 0; i < 12; ++i)
            {
                Points[i * 2 + 0] = Corners[EdgeIndices[i][0]];
                Points[i * 2 + 1] = Corners[EdgeIndices[i][1]];
            }

            Renderer.Segments(Points, 24, Packed);
        }

        void DebugDrawBounds(b3AABB Aabb, b3HexColor Color, void* Context)
        {
            const FVector3 Min = Box3DUtils::FromB3Vec3(Aabb.lowerBound);
            const FVector3 Max = Box3DUtils::FromB3Vec3(Aabb.upperBound);
            DrawBoxWireframe(RendererFrom(Context), (Min + Max) * 0.5f, (Max - Min) * 0.5f, FQuat::Identity(), PackDebugColor(Color));
        }

        void DebugDrawBox(b3Vec3 Extents, b3WorldTransform Transform, b3HexColor Color, void* Context)
        {
            DrawBoxWireframe(RendererFrom(Context), Box3DUtils::FromB3Vec3(Transform.p), Box3DUtils::FromB3Vec3(Extents),
                             Box3DUtils::FromB3Quat(Transform.q), PackDebugColor(Color));
        }

        void DebugDrawString(b3Pos /*P*/, const char* /*Text*/, b3HexColor /*Color*/, void* /*Context*/)
        {
        }
    }

    b3DebugDraw FBox3DPhysicsContext::MakeDebugDraw()
    {
        b3DebugDraw Draw = b3DefaultDebugDraw();

        Draw.DrawShapeFcn = &DebugDrawShape;
        Draw.DrawSegmentFcn = &DebugDrawSegment;
        Draw.DrawTransformFcn = &DebugDrawTransform;
        Draw.DrawPointFcn = &DebugDrawPoint;
        Draw.DrawSphereFcn = &DebugDrawSphere;
        Draw.DrawCapsuleFcn = &DebugDrawCapsule;
        Draw.DrawBoundsFcn = &DebugDrawBounds;
        Draw.DrawBoxFcn = &DebugDrawBox;
        Draw.DrawStringFcn = &DebugDrawString;

        Draw.drawShapes = CVarPhysicsDebugShapes.GetValue();
        Draw.drawJoints = CVarPhysicsDebugJoints.GetValue();
        Draw.drawJointExtras = CVarPhysicsDebugJointExtras.GetValue();
        Draw.drawBounds = CVarPhysicsDebugAABB.GetValue();
        Draw.drawMass = CVarPhysicsDebugMass.GetValue();
        Draw.drawSleep = CVarPhysicsDebugSleep.GetValue();
        Draw.drawBodyNames = CVarPhysicsDebugBodyNames.GetValue();
        Draw.drawContacts = CVarPhysicsDebugContacts.GetValue();
        Draw.drawContactNormals = CVarPhysicsDebugContactNormals.GetValue();
        Draw.drawContactForces = CVarPhysicsDebugContactForces.GetValue();
        Draw.drawContactFeatures = CVarPhysicsDebugContactFeatures.GetValue();
        Draw.drawGraphColors = CVarPhysicsDebugGraphColors.GetValue();
        Draw.drawIslands = CVarPhysicsDebugIslands.GetValue();

        return Draw;
    }

    bool FBox3DPhysicsContext::IsDebugDrawEnabled()
    {
        return CVarPhysicsDebug.GetValue();
    }

    void FBox3DDebugRenderer::DrawWorld(b3WorldId WorldId, CWorld* InWorld)
    {
        LUMINA_PROFILE_SCOPE();

        World = InWorld;

        const FDebugDrawState* State = DebugDraw::GetState(World);
        if (State == nullptr || !State->bEnabled || !State->bHasView)
        {
            return;
        }

        DrawState = State;
        SetImmediateSink(DebugDraw::GetLines(World));

        b3DebugDraw Draw = FBox3DPhysicsContext::MakeDebugDraw();
        Draw.context = this;

        float Reach = Math::Max(CVarPhysicsDebugDrawDistance.GetValue(), 0.0f);
        if (State->MaxDistanceSq > 0.0f)
        {
            const float ViewReach = Math::Sqrt(State->MaxDistanceSq);
            Reach = Reach > 0.0f ? Math::Min(Reach, ViewReach) : ViewReach;
        }

        // Box3D defaults this box to 100 units about the world origin, which silently drops a distant scene.
        const FVector3 Extent(Reach > 0.0f ? Reach : B3_HUGE);
        Draw.drawingBounds = b3AABB{ Box3DUtils::ToB3Vec3(State->ViewOrigin - Extent),
                                     Box3DUtils::ToB3Vec3(State->ViewOrigin + Extent) };

        b3World_Draw(WorldId, &Draw, UINT64_MAX);

        SetImmediateSink(nullptr);
        DrawState = nullptr;
    }

    void FBox3DPhysicsContext::Initialize()
    {
        b3SetAllocator(&Box3DAllocate, &Box3DFree);
        b3SetAssertFcn(&Box3DAssertFailed);
        b3SetLogFcn(&Box3DLog);

        Box3DData = MakeUnique<FBox3DData>();

        const b3Version Version = b3GetVersion();
        LOG_DISPLAY("[Box3D] Physics initialized (version {}.{}.{}, {} precision).",
            Version.major, Version.minor, Version.revision, b3IsDoublePrecision() ? "double" : "single");
    }

    void FBox3DPhysicsContext::Shutdown()
    {
        Box3DData.reset();
    }

    TUniquePtr<IPhysicsScene> FBox3DPhysicsContext::CreatePhysicsScene(CWorld* World)
    {
        return MakeUnique<FBox3DPhysicsScene>(World);
    }

    FBox3DDebugRenderer* FBox3DPhysicsContext::GetDebugRenderer()
    {
        return Box3DData ? &Box3DData->DebugRenderer : nullptr;
    }
}
