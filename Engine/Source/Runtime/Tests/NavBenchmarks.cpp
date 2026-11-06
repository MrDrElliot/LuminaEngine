#include "gtest/gtest.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "AI/Navigation/NavMesh.h"
#include "AI/Navigation/NavMeshBuilder.h"
#include "AI/Navigation/NavTypes.h"
#include "Config/NavigationSettings.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/NavModifierComponent.h"
#include "NavWorldTestUtil.h"

// Run with --gtest_also_run_disabled_tests --gtest_filter=NavBenchmark.* and LUMINA_NAV_BENCH_SIZE as the world edge in meters.

using namespace Lumina;

namespace
{
    struct FOpenWorldSpec
    {
        float  Size        = 1024.0f;
        float  TerrainStep = 2.0f;
        int32  Buildings   = 0;
        int32  Rocks       = 0;
        uint32 Seed        = 7;
    };

    // Deterministic, so two runs of a benchmark measure the same world.
    struct FRandom
    {
        uint64 State;

        explicit FRandom(uint64 Seed) : State(Seed * 0x9E3779B97F4A7C15ull + 1) {}

        uint32 Next()
        {
            State ^= State << 13;
            State ^= State >> 7;
            State ^= State << 17;
            return (uint32)(State >> 16);
        }

        float Range(float Min, float Max)
        {
            return Min + (Max - Min) * ((float)(Next() & 0xFFFFFF) / (float)0xFFFFFF);
        }
    };

    // Rolling hills steep enough in places to break the walkable surface into islands, as real terrain does.
    float TerrainHeight(float X, float Z)
    {
        return 18.0f * std::sin(X * 0.004f) * std::cos(Z * 0.0035f)
             + 6.0f * std::sin(X * 0.017f + 1.3f) * std::sin(Z * 0.021f)
             + 1.5f * std::sin(X * 0.09f) * std::cos(Z * 0.07f + 0.4f);
    }

    void AddTerrain(FNavBuildInput& In, const FOpenWorldSpec& Spec)
    {
        const int32 Quads = (int32)(Spec.Size / Spec.TerrainStep);
        const float Half = Spec.Size * 0.5f;
        const uint32 Base = (uint32)In.Vertices.Num();
        for (int32 Row = 0; Row <= Quads; ++Row)
        {
            for (int32 Col = 0; Col <= Quads; ++Col)
            {
                const float X = -Half + (float)Col * Spec.TerrainStep;
                const float Z = -Half + (float)Row * Spec.TerrainStep;
                In.Vertices.Emplace(X, TerrainHeight(X, Z), Z);
            }
        }
        const uint32 Stride = (uint32)Quads + 1;
        for (uint32 Row = 0; Row < (uint32)Quads; ++Row)
        {
            for (uint32 Col = 0; Col < (uint32)Quads; ++Col)
            {
                const uint32 A = Base + Row * Stride + Col;
                const uint32 B = A + 1;
                const uint32 C = A + Stride;
                const uint32 D = C + 1;
                In.Indices.Append({ A, C, B, B, C, D });
            }
        }
    }

    // A yawed box standing on the terrain, wound so its top faces up like any authored collider.
    void AddBox(FNavBuildInput& In, float X, float Z, const FVector3& Half, float Yaw)
    {
        const float Y = TerrainHeight(X, Z) + Half.y - 0.5f;
        const float C = std::cos(Yaw);
        const float S = std::sin(Yaw);
        const uint32 Base = (uint32)In.Vertices.Num();
        for (int32 Corner = 0; Corner < 8; ++Corner)
        {
            const float LX = (Corner & 1) ? Half.x : -Half.x;
            const float LY = (Corner & 2) ? Half.y : -Half.y;
            const float LZ = (Corner & 4) ? Half.z : -Half.z;
            In.Vertices.Emplace(X + LX * C - LZ * S, Y + LY, Z + LX * S + LZ * C);
        }
        constexpr uint32 Faces[12][3] = {
            {2, 6, 7}, {2, 7, 3}, {0, 1, 5}, {0, 5, 4}, {0, 4, 6}, {0, 6, 2},
            {1, 3, 7}, {1, 7, 5}, {4, 5, 7}, {4, 7, 6}, {0, 2, 3}, {0, 3, 1},
        };
        for (const auto& Face : Faces)
        {
            In.Indices.Append({ Base + Face[0], Base + Face[1], Base + Face[2] });
        }
    }

    FNavBuildInput MakeOpenWorld(const FOpenWorldSpec& Spec)
    {
        FNavBuildInput In;
        In.Settings.TileSizeVoxels = 128;
        AddTerrain(In, Spec);

        FRandom Random(Spec.Seed);
        const float Half = Spec.Size * 0.5f - 8.0f;
        for (int32 i = 0; i < Spec.Buildings; ++i)
        {
            AddBox(In, Random.Range(-Half, Half), Random.Range(-Half, Half),
                FVector3(Random.Range(3.0f, 9.0f), Random.Range(3.0f, 8.0f), Random.Range(3.0f, 9.0f)), Random.Range(0.0f, 3.14f));
        }
        for (int32 i = 0; i < Spec.Rocks; ++i)
        {
            AddBox(In, Random.Range(-Half, Half), Random.Range(-Half, Half),
                FVector3(Random.Range(0.4f, 1.6f), Random.Range(0.5f, 1.5f), Random.Range(0.4f, 1.6f)), Random.Range(0.0f, 3.14f));
        }

        In.BoundsMin = FVector3(-Spec.Size * 0.5f, -40.0f, -Spec.Size * 0.5f);
        In.BoundsMax = FVector3( Spec.Size * 0.5f,  40.0f,  Spec.Size * 0.5f);
        return In;
    }

    FOpenWorldSpec SpecFromEnvironment()
    {
        FOpenWorldSpec Spec;
        if (const char* Size = std::getenv("LUMINA_NAV_BENCH_SIZE"))
        {
            Spec.Size = Math::Max(128.0f, (float)std::atof(Size));
        }
        // Density held per square kilometer, so a bigger world is the same world, only more of it.
        const float SquareKm = (Spec.Size * Spec.Size) / 1.0e6f;
        Spec.Buildings = (int32)(1500.0f * SquareKm);
        Spec.Rocks     = (int32)(12000.0f * SquareKm);
        return Spec;
    }

    struct FBakedWorld
    {
        FOpenWorldSpec       Spec;
        FNavBuildOutput      Layout;
        TUniquePtr<FNavMesh> Mesh;
        TVector<FVector3>    Points;
    };

    void Bake(FBakedWorld& World)
    {
        World.Spec = SpecFromEnvironment();
        FNavBuildInput Input = MakeOpenWorld(World.Spec);
        std::printf("[NavBench] world %.0f m, %d verts, %d tris (%d buildings, %d rocks)\n",
            World.Spec.Size, Input.Vertices.Num(), Input.Indices.Num() / 3, World.Spec.Buildings, World.Spec.Rocks);

        PlatformTime::FStopwatch BakeWatch;
        FNavBuildOutput Output;
        const bool bBaked = NavMeshBuilder::BakeSync(Move(Input), Output);
        const double BakeMs = BakeWatch.ElapsedMilliseconds();

        size_t Bytes = 0;
        int32 Walkable = 0;
        for (const SNavTileData& Tile : Output.Tiles)
        {
            Bytes += Tile.Blob.Num();
            Walkable += Tile.Blob.IsEmpty() ? 0 : 1;
        }
        std::printf("[NavBench] bake %s in %.0f ms, %d tiles (%d walkable, %dx%d grid, %.1f m each), %.1f MiB\n",
            bBaked ? "ok" : "FAILED", BakeMs, Output.Tiles.Num(), Walkable, Output.TilesX, Output.TilesY,
            Output.TileWorldSize, (double)Bytes / (1024.0 * 1024.0));

        World.Layout = Output;
        World.Layout.Tiles.Reset();

        PlatformTime::FStopwatch HydrateWatch;
        World.Mesh = MakeUnique<FNavMesh>();
        World.Mesh->Initialize(Output.Origin, Output.TileWorldSize, Math::Max(1, Output.Tiles.Num()), Output.MaxPolysPerTile, Move(Output.Tiles));
        std::printf("[NavBench] hydrate %.0f ms\n", HydrateWatch.ElapsedMilliseconds());

        // Every query starts and ends on the mesh, so a miss means the query failed rather than the sample.
        FRandom Random(World.Spec.Seed + 11);
        const float Half = World.Spec.Size * 0.5f - 4.0f;
        FNavQueryFilter Filter;
        int32 Attempts = 0;
        while (World.Points.Num() < 4096 && Attempts++ < 100000)
        {
            const float X = Random.Range(-Half, Half);
            const float Z = Random.Range(-Half, Half);
            FVector3 OnMesh;
            // Ground only, since a rooftop or a rock top is an island no path from the ground can reach.
            if (World.Mesh->ProjectPoint(FVector3(X, TerrainHeight(X, Z), Z), FVector3(2.0f, 8.0f, 2.0f), Filter, OnMesh)
                && std::fabs(OnMesh.y - TerrainHeight(OnMesh.x, OnMesh.z)) < 0.5f)
            {
                World.Points.Add(OnMesh);
            }
        }
    }

    struct FPathBand
    {
        const char* Name;
        float MinDistance;
        float MaxDistance;
    };

    struct FPathStats
    {
        int32  Succeeded = 0;
        int32  Partial = 0;
        int32  Truncated = 0;
        int32  Corners = 0;
        double Total = 0.0;
        double Worst = 0.0;
        uint64 Fingerprint = 14695981039346656037ull;

        void Add(const FNavPath& Path, bool bFound, double Ms)
        {
            for (const FVector3& Corner : Path.Corners)
            {
                for (int32 Axis = 0; Axis < 3; ++Axis)
                {
                    Fingerprint = (Fingerprint ^ std::bit_cast<uint32>(Corner[Axis])) * 1099511628211ull;
                }
            }
            Total += Ms;
            Worst = Math::Max(Worst, Ms);
            Succeeded += bFound && !Path.bPartial ? 1 : 0;
            Partial += Path.bPartial ? 1 : 0;
            Truncated += Path.bTruncated ? 1 : 0;
            Corners += Path.Corners.Num();
        }

        void Print(const char* Band, const char* Mode, int32 Ran) const
        {
            std::printf("[NavBench] path %-6s %-6s %4d queries, %8.1f us avg, %7.2f ms worst, %5.1f%% full, %5.1f%% partial, %5.1f%% truncated, %.0f corners avg, corners hash %016llx\n",
                Band, Mode, Ran, Ran ? Total * 1000.0 / Ran : 0.0, Worst, Ran ? 100.0 * Succeeded / Ran : 0.0,
                Ran ? 100.0 * Partial / Ran : 0.0, Ran ? 100.0 * Truncated / Ran : 0.0, Ran ? (double)Corners / Ran : 0.0,
                (unsigned long long)Fingerprint);
        }
    };

    float PathLength(const FNavPath& Path)
    {
        float Length = 0.0f;
        for (int32 i = 1; i < Path.Corners.Num(); ++i)
        {
            Length += Math::Length(Path.Corners[i] - Path.Corners[i - 1]);
        }
        return Length;
    }

    // Holds the hierarchical threshold for one query, so the same pair can be asked both ways.
    bool FindPathWithThreshold(const FBakedWorld& World, const FVector3& A, const FVector3& B, float Threshold, FNavPath& Path, double& Ms)
    {
        float& Setting = GetMutableDefault<CNavigationSettings>()->HierarchicalPathDistance;
        const float Saved = Setting;
        Setting = Threshold;
        FNavQueryFilter Filter;
        PlatformTime::FStopwatch Watch;
        const bool bFound = World.Mesh->FindPath(A, B, Filter, Path);
        Ms = Watch.ElapsedMilliseconds();
        Setting = Saved;
        return bFound;
    }

    // Every pair runs as one flat Detour search and as a tiered one, and where both arrive their lengths are compared.
    void RunPathBand(const FBakedWorld& World, const FPathBand& Band, int32 Count)
    {
        const float Tiered = GetDefault<CNavigationSettings>()->HierarchicalPathDistance;
        FRandom Random(World.Spec.Seed + 101);
        FPathStats Direct, Hierarchical;
        int32 Ran = 0, Compared = 0, Lost = 0;
        double LengthRatio = 0.0;
        int32 Attempts = 0;
        while (Ran < Count && Attempts < Count * 200)
        {
            ++Attempts;
            const FVector3& A = World.Points[Random.Next() % World.Points.Num()];
            const FVector3& B = World.Points[Random.Next() % World.Points.Num()];
            const float Distance = std::sqrt((A.x - B.x) * (A.x - B.x) + (A.z - B.z) * (A.z - B.z));
            if (Distance < Band.MinDistance || Distance >= Band.MaxDistance)
            {
                continue;
            }

            FNavPath DirectPath, TieredPath;
            double DirectMs = 0.0, TieredMs = 0.0;
            const bool bDirect = FindPathWithThreshold(World, A, B, 0.0f, DirectPath, DirectMs);
            const bool bTiered = FindPathWithThreshold(World, A, B, Tiered, TieredPath, TieredMs);
            Direct.Add(DirectPath, bDirect, DirectMs);
            Hierarchical.Add(TieredPath, bTiered, TieredMs);
            ++Ran;

            Lost += DirectPath.Result == ENavPathResult::Success && TieredPath.Result != ENavPathResult::Success ? 1 : 0;
            if (DirectPath.Result == ENavPathResult::Success && TieredPath.Result == ENavPathResult::Success)
            {
                LengthRatio += PathLength(TieredPath) / Math::Max(0.01f, PathLength(DirectPath));
                ++Compared;
            }
        }
        Direct.Print(Band.Name, "direct", Ran);
        Hierarchical.Print(Band.Name, "tiered", Ran);
        if (Compared > 0)
        {
            std::printf("[NavBench] path %-6s tiered length is %.3fx direct over %d pairs both completed, %d direct completed that tiered did not\n",
                Band.Name, LengthRatio / Compared, Compared, Lost);
        }
    }

    void RunPathQueries(const FBakedWorld& World)
    {
        const FPathBand Bands[] = {
            { "short",  5.0f,    50.0f },
            { "medium", 50.0f,   250.0f },
            { "long",   250.0f,  1000.0f },
            { "huge",   1000.0f, 1.0e9f },
        };
        for (const FPathBand& Band : Bands)
        {
            RunPathBand(World, Band, 400);
        }
    }

    // The shape of a crowd repathing at once, every worker asking at the same moment.
    void RunParallelPaths(const FBakedWorld& World)
    {
        constexpr uint32 Queries = 8192;
        std::atomic<int32> Unavailable{ 0 };
        PlatformTime::FStopwatch Watch;
        Jobs::ParallelFor(Queries, [&](uint32 Index)
        {
            FRandom Random(Index * 2654435761u + 3);
            const FVector3& A = World.Points[Random.Next() % World.Points.Num()];
            const FVector3& B = World.Points[Random.Next() % World.Points.Num()];
            FNavPath Path;
            FNavQueryFilter Filter;
            World.Mesh->FindPath(A, B, Filter, Path);
            if (Path.bQueryUnavailable)
            {
                Unavailable.fetch_add(1, std::memory_order_relaxed);
            }
        });
        const double Ms = Watch.ElapsedMilliseconds();
        std::printf("[NavBench] parallel %u random paths in %.1f ms (%.0f paths/s), %d refused for want of a query slot\n",
            Queries, Ms, Queries / (Ms / 1000.0), Unavailable.load());
    }

    void RunPointQueries(const FBakedWorld& World)
    {
        FNavQueryFilter Filter;
        FRandom Random(World.Spec.Seed + 202);

        constexpr int32 Projects = 100000;
        PlatformTime::FStopwatch ProjectWatch;
        int32 Hits = 0;
        for (int32 i = 0; i < Projects; ++i)
        {
            const FVector3& P = World.Points[Random.Next() % World.Points.Num()];
            FVector3 Out;
            Hits += World.Mesh->ProjectPoint(P + FVector3(0.5f, 1.0f, -0.5f), FVector3(2.0f, 8.0f, 2.0f), Filter, Out) ? 1 : 0;
        }
        std::printf("[NavBench] project %d points, %.2f us avg, %d hit\n", Projects, ProjectWatch.ElapsedMilliseconds() * 1000.0 / Projects, Hits);

        constexpr int32 Rays = 20000;
        PlatformTime::FStopwatch RayWatch;
        int32 Blocked = 0;
        for (int32 i = 0; i < Rays; ++i)
        {
            const FVector3& A = World.Points[Random.Next() % World.Points.Num()];
            const FVector3 B = A + FVector3(Random.Range(-40.0f, 40.0f), 0.0f, Random.Range(-40.0f, 40.0f));
            SNavRaycastResult Hit;
            World.Mesh->Raycast(A, B, Filter, Hit);
            Blocked += Hit.bHit ? 1 : 0;
        }
        std::printf("[NavBench] raycast %d rays up to 40 m, %.2f us avg, %d blocked\n", Rays, RayWatch.ElapsedMilliseconds() * 1000.0 / Rays, Blocked);

        constexpr int32 Randoms = 20000;
        PlatformTime::FStopwatch RandomWatch;
        for (int32 i = 0; i < Randoms; ++i)
        {
            FVector3 Out;
            World.Mesh->FindRandomPoint(World.Points[Random.Next() % World.Points.Num()], 30.0f, Filter, Out);
        }
        std::printf("[NavBench] random point %d within 30 m, %.2f us avg\n", Randoms, RandomWatch.ElapsedMilliseconds() * 1000.0 / Randoms);
    }

    // What a hot rebake of a few dirty tiles costs once the world is big, the case dynamic rebuild lives on.
    void RunTileRebake(FBakedWorld& World)
    {
        FNavBuildInput Input = MakeOpenWorld(World.Spec);
        TVector<FNavTileCoord> Coords;
        const int32 CX = World.Layout.TilesX / 2;
        const int32 CY = World.Layout.TilesY / 2;
        for (int32 DY = 0; DY < 4; ++DY)
        {
            for (int32 DX = 0; DX < 4; ++DX)
            {
                Coords.Emplace(CX + DX, CY + DY);
            }
        }

        PlatformTime::FStopwatch Watch;
        TVector<SNavTileData> Baked;
        NavMeshBuilder::BakeTiles(Input, World.Layout, Coords, Baked);
        const double BakeMs = Watch.ElapsedMilliseconds();

        PlatformTime::FStopwatch SwapWatch;
        for (SNavTileData& Tile : Baked)
        {
            World.Mesh->RebuildTile(Tile.X, Tile.Y, Move(Tile.Blob));
        }
        std::printf("[NavBench] rebake 16 tiles from the whole world's %d tris in %.1f ms, swap %.2f ms\n",
            Input.Indices.Num() / 3, BakeMs, SwapWatch.ElapsedMilliseconds());

        // The first long path after a change relabels the cluster islands, which every later one reuses.
        FNavQueryFilter Filter;
        const FVector3 From = World.Points[0];
        const FVector3 To = World.Points.Last();
        for (const char* When : { "first", "second" })
        {
            FNavPath Path;
            PlatformTime::FStopwatch PathWatch;
            World.Mesh->FindPath(From, To, Filter, Path);
            std::printf("[NavBench] %s path after the rebake, %.0f m apart, %.2f ms\n", When, Math::Length(To - From), PathWatch.ElapsedMilliseconds());
        }
    }
}

// One test, so the world bakes once and every measurement reads the same mesh.
TEST(NavBenchmark, DISABLED_OpenWorld)
{
    FBakedWorld World;
    Bake(World);
    ASSERT_TRUE(World.Mesh && World.Mesh->IsReady());
    ASSERT_FALSE(World.Points.IsEmpty());

    RunPathQueries(World);
    RunParallelPaths(World);
    RunPointQueries(World);
    RunTileRebake(World);
}

// The engine side of open world navigation, where every collider in the level is a source the system tracks.
TEST(NavBenchmark, DISABLED_WorldSources)
{
    float Size = 2048.0f;
    if (const char* Env = std::getenv("LUMINA_NAV_BENCH_SIZE"))
    {
        Size = Math::Max(256.0f, (float)std::atof(Env));
    }
    int32 ObstacleCount = 100000;
    if (const char* Env = std::getenv("LUMINA_NAV_BENCH_SOURCES"))
    {
        ObstacleCount = Math::Max(0, std::atoi(Env));
    }

    NavTest::FNavWorld World;
    PlatformTime::FStopwatch BuildWatch;
    World.Create(Size);
    TVector<ECS::FEntity> Obstacles;
    Obstacles.Reserve(ObstacleCount);
    FRandom Random(99);
    const float Half = Size * 0.5f - 4.0f;
    for (int32 i = 0; i < ObstacleCount; ++i)
    {
        const FVector3 Extent(Random.Range(0.4f, 3.0f), Random.Range(0.5f, 2.5f), Random.Range(0.4f, 3.0f));
        Obstacles.Add(NavTest::SpawnBox(World.Registry(), FVector3(Random.Range(-Half, Half), Extent.y, Random.Range(-Half, Half)), Extent));
    }
    World.Nav().Settings.TileSizeVoxels = 128;
    std::printf("[NavBench] world %.0f m with %d obstacles built in %.0f ms\n", Size, ObstacleCount, BuildWatch.ElapsedMilliseconds());

    PlatformTime::FStopwatch StartWatch;
    World.Start();
    std::printf("[NavBench] system start, seeding the source index, %.0f ms\n", StartWatch.ElapsedMilliseconds());

    PlatformTime::FStopwatch BakeWatch;
    const bool bReady = World.WaitReady(600000.0);
    std::printf("[NavBench] bake to ready %s in %.0f ms, %d tiles\n", bReady ? "ok" : "TIMED OUT", BakeWatch.ElapsedMilliseconds(), World.Nav().Tiles.Num());
    ASSERT_TRUE(bReady);

    double Idle = 0.0, IdleWorst = 0.0;
    constexpr int32 IdleTicks = 20;
    for (int32 i = 0; i < IdleTicks; ++i)
    {
        const double Ms = World.Tick();
        Idle += Ms;
        IdleWorst = Math::Max(IdleWorst, Ms);
    }
    std::printf("[NavBench] idle tick with every source unchanged, %.2f ms avg, %.2f ms worst\n", Idle / IdleTicks, IdleWorst);

    // A handful of things moving each frame, a door, a cart, a crate kicked across a yard.
    double Moving = 0.0, MovingWorst = 0.0;
    constexpr int32 MoveTicks = 20;
    for (int32 i = 0; i < MoveTicks; ++i)
    {
        for (int32 Moved = 0; Moved < 10; ++Moved)
        {
            STransformComponent& Transform = World.Registry().Get<STransformComponent>(Obstacles[Random.Next() % Obstacles.Num()]);
            Transform.SetLocation(Transform.GetLocation() + FVector3(Random.Range(-2.0f, 2.0f), 0.0f, Random.Range(-2.0f, 2.0f)));
        }
        const double Ms = World.Tick();
        Moving += Ms;
        MovingWorst = Math::Max(MovingWorst, Ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("[NavBench] tick with 10 sources moving, %.2f ms avg, %.2f ms worst\n", Moving / MoveTicks, MovingWorst);

    PlatformTime::FStopwatch DrainWatch;
    const bool bDrained = World.WaitSettled(120000.0);
    std::printf("[NavBench] dirty tiles drained %s in %.0f ms\n", bDrained ? "ok" : "TIMED OUT", DrainWatch.ElapsedMilliseconds());

    World.Stop();
}

namespace
{
    int32 EnvInt(const char* Name, int32 Default)
    {
        const char* Value = std::getenv(Name);
        return Value ? std::atoi(Value) : Default;
    }

    struct FFrameSeries
    {
        TVector<double> Samples;

        void Print(const char* Name) const
        {
            TVector<double> Sorted = Samples;
            Algo::Sort(Sorted);
            double Sum = 0.0;
            for (double Sample : Sorted)
            {
                Sum += Sample;
            }
            const int32 N = Sorted.Num();
            std::printf("[NavHitch] %-24s avg %6.2f ms, p99 %6.2f ms, worst %6.2f ms\n", Name, N ? Sum / N : 0.0,
                N ? Sorted[static_cast<int32>(0.99 * (N - 1))] : 0.0, N ? Sorted.Last() : 0.0);
        }
    };

    // Fixed work spread over every worker, so its wall time grows when something else holds them.
    double ProbeWorkers()
    {
        const uint32 Chunks = Math::Max(1u, Jobs::GetNumWorkers()) * 4;
        PlatformTime::FStopwatch Watch;
        Jobs::ParallelFor(Chunks, [](uint32)
        {
            PlatformTime::FStopwatch Spin;
            while (Spin.ElapsedMilliseconds() < 0.1)
            {
            }
        }, 1);
        return Watch.ElapsedMilliseconds();
    }

    struct FMover
    {
        ECS::FEntity Entity;
        FVector3     Center;
        float        Radius;
        float        Angle;
    };
}

// A crowd standing on its paths while obstacles drift through them and another thread keeps asking for paths, timed the way a frame feels it.
TEST(NavBenchmark, DISABLED_Hitch)
{
    const float Size = (float)EnvInt("LUMINA_NAV_BENCH_SIZE", 1024);
    const int32 AgentCount = EnvInt("LUMINA_NAV_BENCH_AGENTS", 2000);
    const int32 MoverCount = EnvInt("LUMINA_NAV_BENCH_MOVERS", 40);
    const bool bModifierMovers = EnvInt("LUMINA_NAV_BENCH_MODIFIERS", 0) != 0;
    const int32 Frames = EnvInt("LUMINA_NAV_BENCH_FRAMES", 600);
    const bool bGameplayQueries = EnvInt("LUMINA_NAV_BENCH_QUERY_THREAD", 1) != 0;
    constexpr double kFrameMs = 1000.0 / 60.0;
    if (EnvInt("LUMINA_NAV_BENCH_TIMINGS", 0) != 0)
    {
        FConsoleRegistry::Get().SetValueFromString("Nav.Debug.Timings", "1");
    }

    NavTest::FNavWorld World;
    World.Create(Size, 256.0f, true);
    ECS::FRegistry& Registry = World.Registry();
    FRandom Random(5);
    const float Half = Size * 0.5f - 8.0f;
    auto Ground = [](float X, float Z) { return FVector3(X, NavTest::GroundHeight(X, Z), Z); };

    const int32 Rocks = (int32)(Size * Size / 400.0f);
    for (int32 i = 0; i < Rocks; ++i)
    {
        const FVector3 Extent(Random.Range(0.4f, 2.0f), Random.Range(0.4f, 1.5f), Random.Range(0.4f, 2.0f));
        NavTest::SpawnBox(Registry, Ground(Random.Range(-Half, Half), Random.Range(-Half, Half)) + FVector3(0.0f, Extent.y - 0.3f, 0.0f), Extent);
    }
    World.Nav().Settings.TileSizeVoxels = 128;
    std::printf("[NavHitch] %.0f m terrain, %d rocks, %d agents, %d moving %s\n", Size, Rocks, AgentCount, MoverCount, bModifierMovers ? "modifiers" : "box colliders");

    World.Start(true);
    ASSERT_TRUE(World.WaitReady(600000.0));
    std::printf("[NavHitch] baked %d tiles\n", World.Nav().Tiles.Num());

    for (int32 i = 0; i < AgentCount; ++i)
    {
        const FVector3 From = Ground(Random.Range(-Half, Half), Random.Range(-Half, Half));
        const float Heading = Random.Range(0.0f, 6.283f);
        const float Distance = Random.Range(50.0f, 400.0f);
        const FVector3 To = Ground(Math::Clamp(From.x + std::cos(Heading) * Distance, -Half, Half), Math::Clamp(From.z + std::sin(Heading) * Distance, -Half, Half));

        const ECS::FEntity Agent = Registry.Create();
        FTransform Transform;
        Transform.SetLocation(From + FVector3(0.0f, 0.2f, 0.0f));
        Registry.Emplace<STransformComponent>(Agent, Transform);
        Registry.Emplace<SPathFollowComponent>(Agent).SetTargetLocation(To);
    }

    // The first wave of paths, which every agent owes at once.
    {
        FFrameSeries Wave;
        int32 Settled = 0;
        for (int32 Frame = 0; Frame < 600 && Settled < 10; ++Frame)
        {
            const double Ms = World.TickFollow();
            Wave.Samples.Add(Ms);
            Settled = Ms < 0.5 ? Settled + 1 : 0;
        }
        std::printf("[NavHitch] first paths for the crowd took %d frames\n", Wave.Samples.Num());
        Wave.Print("first paths frame");
    }

    TVector<FMover> Movers;
    for (int32 i = 0; i < MoverCount; ++i)
    {
        FMover Mover{ ECS::NullEntity, Ground(Random.Range(-Half + 30.0f, Half - 30.0f), Random.Range(-Half + 30.0f, Half - 30.0f)), Random.Range(8.0f, 25.0f), Random.Range(0.0f, 6.283f) };
        const FVector3 Start = Mover.Center + FVector3(std::cos(Mover.Angle) * Mover.Radius, 0.0f, std::sin(Mover.Angle) * Mover.Radius);
        if (bModifierMovers)
        {
            Mover.Entity = Registry.Create();
            FTransform Transform;
            Transform.SetLocation(Start);
            Registry.Emplace<STransformComponent>(Mover.Entity, Transform);
            SNavModifierComponent& Modifier = Registry.Emplace<SNavModifierComponent>(Mover.Entity);
            Modifier.Area = ENavArea::Null;
            Modifier.Extents = FVector3(2.0f, 3.0f, 2.0f);
        }
        else
        {
            Mover.Entity = NavTest::SpawnBox(Registry, Start + FVector3(0.0f, 0.7f, 0.0f), FVector3(2.0f, 0.7f, 2.0f));
        }
        Movers.Add(Mover);
    }

    // Other gameplay asking for long paths from its own thread, the way a script or a spawner would.
    std::atomic<bool> bStopQueries{ false };
    std::atomic<int32> GameplayQueries{ 0 };
    FNavMesh* Mesh = World.Nav().Runtime.Mesh.get();
    FThread QueryThread([&]
    {
        FRandom QueryRandom(77);
        FNavQueryFilter Filter;
        while (bGameplayQueries && !bStopQueries.load(std::memory_order_relaxed))
        {
            FNavPath Path;
            Mesh->FindPath(Ground(QueryRandom.Range(-Half, Half), QueryRandom.Range(-Half, Half)),
                           Ground(QueryRandom.Range(-Half, Half), QueryRandom.Range(-Half, Half)), Filter, Path);
            GameplayQueries.fetch_add(1, std::memory_order_relaxed);
        }
    });

    FFrameSeries NavTick, FollowTick, Probe, Frame;
    const uint64 EpochBefore = Mesh->GetTopologyEpoch();
    PlatformTime::FStopwatch RunWatch;
    for (int32 FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
    {
        PlatformTime::FStopwatch FrameWatch;
        for (FMover& Mover : Movers)
        {
            Mover.Angle += 3.0f / 60.0f / Mover.Radius;
            const FVector3 At = Mover.Center + FVector3(std::cos(Mover.Angle) * Mover.Radius, 0.0f, std::sin(Mover.Angle) * Mover.Radius);
            Registry.Get<STransformComponent>(Mover.Entity).SetLocation(Ground(At.x, At.z) + FVector3(0.0f, bModifierMovers ? 0.0f : 0.7f, 0.0f));
        }
        NavTick.Samples.Add(World.Tick());
        FollowTick.Samples.Add(World.TickFollow());
        Probe.Samples.Add(ProbeWorkers());
        const double Used = FrameWatch.ElapsedMilliseconds();
        Frame.Samples.Add(Used);
        if (Used < kFrameMs)
        {
            std::this_thread::sleep_for(std::chrono::microseconds((int64)((kFrameMs - Used) * 1000.0)));
        }
    }
    const double RunSeconds = RunWatch.ElapsedMilliseconds() / 1000.0;
    bStopQueries.store(true);
    QueryThread.Join();

    NavTick.Print("nav system tick");
    FollowTick.Print("path follow tick");
    Probe.Print("worker probe, 0.4 ideal");
    Frame.Print("frame work");
    std::printf("[NavHitch] %.1f tile changes/s, %.0f gameplay paths/s over %.1f s\n",
        (double)(Mesh->GetTopologyEpoch() - EpochBefore) / RunSeconds, GameplayQueries.load() / RunSeconds, RunSeconds);
    if (const TSharedPtr<FNavTileCache>& Cache = World.Nav().Runtime.TileCache)
    {
        std::printf("[NavHitch] tile cache %d hits, %d misses\n", Cache->GetHits(), Cache->GetMisses());
    }
    World.Stop();
}
