#include "RuntimePCH.h"

#include "World/Subsystems/NavigationSubsystem.h"
#include "World/World.h"

#include "World/Entity/Systems/NavMeshSystem.h"

namespace Lumina
{
    bool CNavigationSubsystem::IsReady(FName Agent)
    {
        return Nav::IsReady(GetWorld(), Agent);
    }

    int32 CNavigationSubsystem::FindPath(FVector3 Start, FVector3 End, FVector3* Corners,
        int32 MaxCorners, FName Agent)
    {
        FNavPath Path;
        if (Corners == nullptr || MaxCorners <= 0 || !Nav::FindPath(GetWorld(), Start, End, MaxCorners, Path, Agent)
            || !Path.bValid)
        {
            return 0;
        }

        int32 Count = Path.Corners.Num();
        if (Count > MaxCorners)
        {
            Count = MaxCorners;
        }
        for (int32 i = 0; i < Count; ++i)
        {
            Corners[i] = Path.Corners[i];
        }
        return Count;
    }

    bool CNavigationSubsystem::IsPathPartial(FVector3 Start, FVector3 End, int32 MaxCorners, FName Agent)
    {
        FNavPath Path;
        if (!Nav::FindPath(GetWorld(), Start, End, MaxCorners, Path, Agent) || !Path.bValid)
        {
            return true;
        }
        return Path.bPartial || Path.bTruncated || Path.Corners.Num() > MaxCorners;
    }

    ENavPathResult CNavigationSubsystem::GetPathResult(FVector3 Start, FVector3 End, int32 MaxCorners, FName Agent)
    {
        FNavPath Path;
        Nav::FindPath(GetWorld(), Start, End, MaxCorners, Path, Agent);

        // A route longer than the caller's buffer arrives valid but short, which is a truncation either way.
        if (Path.bValid && Path.Result == ENavPathResult::Success && MaxCorners > 0
            && Path.Corners.Num() > MaxCorners)
        {
            return ENavPathResult::Truncated;
        }
        return Path.Result;
    }

    SNavPoint CNavigationSubsystem::ProjectPoint(FVector3 Point, FVector3 Extents, FName Agent)
    {
        SNavPoint Result;
        FVector3 Projected;
        if (Nav::ProjectPoint(GetWorld(), Point, Extents, Projected, Agent))
        {
            Result.bFound = true;
            Result.Point = Projected;
        }
        return Result;
    }

    SNavRaycastResult CNavigationSubsystem::Raycast(FVector3 Start, FVector3 End, FName Agent)
    {
        SNavRaycastResult Result;
        Nav::Raycast(GetWorld(), Start, End, Result, Agent);
        return Result;
    }

    bool CNavigationSubsystem::IsWalkableLine(FVector3 From, FVector3 To, FName Agent)
    {
        return Nav::IsWalkableLine(GetWorld(), From, To, Agent);
    }

    SNavPoint CNavigationSubsystem::FindRandomReachablePoint(FVector3 Origin, float Radius, FName Agent)
    {
        SNavPoint Result;
        FVector3 Found;
        if (Nav::FindRandomReachablePoint(GetWorld(), Origin, Radius, Found, Agent))
        {
            Result.bFound = true;
            Result.Point = Found;
        }
        return Result;
    }

    bool CNavigationSubsystem::IsReachable(FVector3 From, FVector3 To, FName Agent)
    {
        return Nav::IsReachable(GetWorld(), From, To, Agent);
    }

    float CNavigationSubsystem::PathLength(FVector3 From, FVector3 To, FName Agent)
    {
        return Nav::PathLength(GetWorld(), From, To, Agent);
    }

    int32 CNavigationSubsystem::RequestRebuild()
    {
        return Nav::RequestRebuild(GetWorld());
    }

    void CNavigationSubsystem::DrawPath(FVector3 From, FVector3 To, FVector4 Color, float Duration, FName Agent)
    {
        Nav::DrawDebugPath(GetWorld(), From, To, Color, Duration, Agent);
    }
}
