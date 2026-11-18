#pragma once

#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "World/Entity/Systems/SystemResources.h"
#include "LagCompensation.generated.h"

namespace Lumina
{
    // One hitbox at one moment, as the world-space segment the capsule is built around.
    struct FLagCompensationSample
    {
        ECS::FEntity Entity = ECS::NullEntity;
        FVector3     A      = FVector3(0.0f);
        FVector3     B      = FVector3(0.0f);
        float        Radius = 0.0f;

        // Half the segment plus the radius, filled when the frame is finished so a shot rejects the hitbox without a square root.
        float        BoundRadius = 0.0f;
    };

    // Every hitbox at one server time, sorted by entity so two frames line up in one merge.
    struct FLagCompensationFrame
    {
        double                          Time = -1.0;
        TVector<FLagCompensationSample> Samples;

        // How far any hitbox moved since the frame before, which grows each bound when a shot falls between the two.
        float                           MaxMoveFromPrevious = 0.0f;

        NODISCARD const FLagCompensationSample* Find(ECS::FEntity Entity) const;
    };

    // The host's hitbox history for the rewind window, a ring whose frames keep their buffers so recording never allocates once warm.
    struct NETWORKINGRUNTIME_API FLagCompensationHistory : ECS::FResource
    {
        TVector<FLagCompensationFrame> Frames;
        int32  Newest = -1;
        int32  Count  = 0;
        double MaxRewindSeconds = 0.35;

        // Samples need not be sorted. A frame older than the window plus one is dropped.
        void Record(double Time, TSpan<const FLagCompensationSample> Samples);

        // A frame recorded at Time, which the caller fills and then calls FinishRecord on.
        FLagCompensationFrame& BeginRecord(double Time);
        void FinishRecord();

        NODISCARD double GetNewestTime() const;
        NODISCARD double GetOldestTime() const;

        // Within the window, so a client cannot ask for further back than the host is willing to rewind.
        NODISCARD double ClampViewTime(double ViewTime) const;

        NODISCARD const FLagCompensationFrame& FrameAt(int32 Age) const;

    private:

        void EnsureCapacity(double Time);
    };

    REFLECT()
    struct NETWORKINGRUNTIME_API SLagCompensatedHit
    {
        GENERATED_BODY()

        PROPERTY()
        bool bHit = false;

        PROPERTY()
        ECS::FEntity Entity = ECS::NullEntity;

        PROPERTY()
        FVector3 Point = FVector3(0.0f);

        PROPERTY()
        FVector3 Normal = FVector3(0.0f, 1.0f, 0.0f);

        PROPERTY()
        float Distance = 0.0f;
    };

    namespace LagCompensation
    {
        // A ray, or with Thickness a swept sphere, against every hitbox where it was at ViewTime. Ignore is usually the shooter.
        NETWORKINGRUNTIME_API SLagCompensatedHit Raycast(const FLagCompensationHistory& History, double ViewTime, const FVector3& Origin,
            const FVector3& Direction, float MaxDistance, float Thickness, ECS::FEntity Ignore);

        // Every hitbox a sphere touched at ViewTime, for melee and explosions.
        NETWORKINGRUNTIME_API void OverlapSphere(const FLagCompensationHistory& History, double ViewTime, const FVector3& Center, float Radius,
            TVector<ECS::FEntity>& Out);
    }
}
