#include "RuntimePCH.h"

#include "NavigationLibrary.h"

#include "World/Entity/Systems/NavMeshSystem.h"

namespace Lumina
{
    bool CNavigationLibrary::IsReady(CWorld* World, FName Agent)
    {
        return Nav::IsReady(World, Agent);
    }

    int32 CNavigationLibrary::FindPath(CWorld* World, FVector3 Start, FVector3 End, FVector3* Corners,
        int32 MaxCorners, FName Agent)
    {
        FNavPath Path;
        if (Corners == nullptr || MaxCorners <= 0 || !Nav::FindPath(World, Start, End, MaxCorners, Path, Agent)
            || !Path.bValid)
        {
            return 0;
        }

        int32 Count = (int32)Path.Corners.size();
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

    bool CNavigationLibrary::IsPathPartial(CWorld* World, FVector3 Start, FVector3 End, int32 MaxCorners, FName Agent)
    {
        FNavPath Path;
        if (!Nav::FindPath(World, Start, End, MaxCorners, Path, Agent) || !Path.bValid)
        {
            return true;
        }
        return Path.bPartial || Path.bTruncated || (int32)Path.Corners.size() > MaxCorners;
    }

    ENavPathResult CNavigationLibrary::GetPathResult(CWorld* World, FVector3 Start, FVector3 End, int32 MaxCorners, FName Agent)
    {
        FNavPath Path;
        Nav::FindPath(World, Start, End, MaxCorners, Path, Agent);

        // A route longer than the caller's buffer arrives valid but short, which is a truncation either way.
        if (Path.bValid && Path.Result == ENavPathResult::Success && MaxCorners > 0
            && (int32)Path.Corners.size() > MaxCorners)
        {
            return ENavPathResult::Truncated;
        }
        return Path.Result;
    }

    FString CNavigationLibrary::DescribePathResult(ENavPathResult Result)
    {
        return FString(ToString(Result));
    }

    FNavPoint CNavigationLibrary::ProjectPoint(CWorld* World, FVector3 Point, FVector3 Extents, FName Agent)
    {
        FNavPoint Result;
        FVector3 Projected;
        if (Nav::ProjectPoint(World, Point, Extents, Projected, Agent))
        {
            Result.bFound = true;
            Result.Point = Projected;
        }
        return Result;
    }

    FNavRaycastResult CNavigationLibrary::Raycast(CWorld* World, FVector3 Start, FVector3 End, FName Agent)
    {
        FNavRaycastResult Result;
        Nav::Raycast(World, Start, End, Result, Agent);
        return Result;
    }

    bool CNavigationLibrary::IsWalkableLine(CWorld* World, FVector3 From, FVector3 To, FName Agent)
    {
        return Nav::IsWalkableLine(World, From, To, Agent);
    }

    FNavPoint CNavigationLibrary::FindRandomReachablePoint(CWorld* World, FVector3 Origin, float Radius, FName Agent)
    {
        FNavPoint Result;
        FVector3 Found;
        if (Nav::FindRandomReachablePoint(World, Origin, Radius, Found, Agent))
        {
            Result.bFound = true;
            Result.Point = Found;
        }
        return Result;
    }

    bool CNavigationLibrary::IsReachable(CWorld* World, FVector3 From, FVector3 To, FName Agent)
    {
        return Nav::IsReachable(World, From, To, Agent);
    }

    float CNavigationLibrary::PathLength(CWorld* World, FVector3 From, FVector3 To, FName Agent)
    {
        return Nav::PathLength(World, From, To, Agent);
    }

    int32 CNavigationLibrary::RequestRebuild(CWorld* World)
    {
        return Nav::RequestRebuild(World);
    }

    void CNavigationLibrary::DrawPath(CWorld* World, FVector3 From, FVector3 To, FVector4 Color, float Duration, FName Agent)
    {
        Nav::DrawDebugPath(World, From, To, Color, Duration, Agent);
    }
}
