#pragma once

#include <box3d/box3d.h>
#include "Physics/Physics.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    class FImmediateLineRenderer;
    class CWorld;
    struct FDebugDrawState;
}

namespace Lumina::Physics
{
    // Local-space wireframe built once per Box3D shape and replayed under the body transform each frame.
    struct FBox3DDebugShape
    {
        TVector<FVector3> LinePoints;

        // Bound of LinePoints about the body origin, so a shape can be frustum tested before it replays.
        float BoundingRadius = 0.0f;
    };

    // Box3D's debug output routed onto the immediate line path, so a drawn edge costs two vertex writes.
    class FBox3DDebugRenderer
    {
    public:

        void DrawWorld(b3WorldId WorldId, CWorld* InWorld);

        FORCEINLINE void SetDrawDuration(float InDuration) { Duration = InDuration; }
        FORCEINLINE void SetImmediateSink(FImmediateLineRenderer* InLines) { Lines = InLines; }
        FORCEINLINE void SetWorld(CWorld* InWorld) { World = InWorld; }

        void Line(const FVector3& From, const FVector3& To, uint32 PackedColor);

        // Points is a flat run of segment endpoint pairs, emitted under one reservation.
        void Segments(const FVector3* Points, int32 PointCount, uint32 PackedColor);
        void Segments(const FVector3* Points, int32 PointCount, const FVector3& Translation, const FQuat& Rotation, uint32 PackedColor);

        // The immediate path culls per source rather than per line, so ask before generating a shape's lines.
        bool ShouldDrawShape(const FVector3& Center, float Radius) const;

        // Cleared, capacity-keeping scratch for the callbacks that tessellate on the fly.
        TVector<FVector3>& TakeScratch();

        static void* CreateDebugShape(const b3DebugShape* Shape, void* UserContext);
        static void DestroyDebugShape(void* UserShape, void* UserContext);

    private:

        double Duration = 0.0;
        CWorld* World = nullptr;
        FImmediateLineRenderer* Lines = nullptr;

        // Valid only for the DrawWorld call that published it.
        const FDebugDrawState* DrawState = nullptr;

        TVector<FVector3> Scratch;
    };

    struct FBox3DData
    {
        FBox3DDebugRenderer DebugRenderer;
        FString LastErrorMessage;
    };

    class FBox3DPhysicsContext : public IPhysicsContext
    {
    public:

        void Initialize() override;
        void Shutdown() override;
        TUniquePtr<IPhysicsScene> CreatePhysicsScene(CWorld* World) override;

        static FBox3DDebugRenderer* GetDebugRenderer();
        static b3DebugDraw MakeDebugDraw();
        static bool IsDebugDrawEnabled();
    };
}
