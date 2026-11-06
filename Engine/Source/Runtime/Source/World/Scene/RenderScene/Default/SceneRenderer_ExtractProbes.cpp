#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace SceneRendererExtractProbesInternal
    {
        TAtomic<uint32> GReflectionProbeRebakeRequests{0};

        FAutoConsoleCommand GCmdRebakeReflectionProbes(
            "r.ReflectionProbes.Rebake",
            "Recapture every reflection probe. Needed after moving world geometry, which does not itself "
            "invalidate a bake (only changing a probe does).",
            []{ RequestReflectionProbeRebake(); });

        // Everything BuildSplineSamples and the header read, so an equal key means an identical upload.
        uint64 SplineDataKey(const SSplineComponent& Spline)
        {
            uint64 Key = Hash::GetHash64(Spline.Points.GetData(), (size_t)Spline.Points.Num() * sizeof(SSplinePoint), 0);
            Key = HashExtractValue(Spline.DefaultUpVector, Key);
            Key = HashExtractValue(Spline.SamplesPerSegment, Key);
            return HashExtractValue(Spline.bClosedLoop, Key);
        }

        uint64 ProbeInputKey(const SReflectionProbeComponent& Probe, const FTransform& World)
        {
            return HashExtractValue(World, HashExtractValue(Probe, 0));
        }

        bool MatrixNearlyEqual(const FMatrix4& A, const FMatrix4& B, float Epsilon)
        {
            for (int32 c = 0; c < 4; ++c)
            {
                for (int32 r = 0; r < 4; ++r)
                {
                    if (Math::Abs(A[c][r] - B[c][r]) > Epsilon)
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    }

    void RequestReflectionProbeRebake()
    {
        SceneRendererExtractProbesInternal::GReflectionProbeRebakeRequests.fetch_add(1, std::memory_order_relaxed);
    }

    void FDefaultSceneRenderer::ScheduleSplineExtract(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        const bool bDataMayHaveChanged = PoolMayHaveChanged(Registry, ECS::GetComponentTypeID<SSplineComponent>(), SplinePoolWatch);

        auto SplineView = Registry.View<ECS::Read<SSplineComponent>, ECS::Read<STransformComponent>, ECS::Without<SDisabledTag>>();
        Jobs::Spawn(Jobs::EJobPriority::Normal, ExtractLatch, [this, &Frame, SplineView, bDataMayHaveChanged]
        {
            ExtractSplines(Frame, SplineView, bDataMayHaveChanged);
        });
    }

    void FDefaultSceneRenderer::ExtractSplines(FFrameData& Frame, const auto& SplineView, bool bDataMayHaveChanged)
    {
        using namespace SceneRendererExtractProbesInternal;

        LUMINA_PROFILE_SECTION("Extract Splines");

        auto& Splines = Frame.Splines.Splines;
        auto& Points  = Frame.Splines.Points;
        auto& Samples = Frame.Splines.Samples;
        Splines.Reset();
        Points.Reset();
        Samples.Reset();

        SplineView.ForEach([&](ECS::FEntity Entity, const SSplineComponent& Spline, const STransformComponent& Transform)
        {
            // The opt-in is the whole point, since pure authoring data must cost no upload.
            if (!Spline.bSendToGPU || Spline.Points.IsEmpty())
            {
                return;
            }

            FSplineExtractCache& Cache = SplineCache.FindOrAdd(Entity);
            Cache.SeenSerial = ExtractSerial;

            // A pool nothing wrote cannot hold new points, so only a possible write pays for hashing them.
            const uint64 DataKey = (bDataMayHaveChanged || !Cache.bValid) ? SplineDataKey(Spline) : Cache.DataKey;
            const uint64 Key     = HashExtractValue(Transform.GetWorldTransformCached(), DataKey);
            if (!Cache.bValid || Cache.Key != Key)
            {
                BuildSplineCache(Cache, Entity, Spline, Transform.GetWorldMatrixCached());
                Cache.Key    = Key;
                Cache.bValid = true;
            }
            Cache.DataKey = DataKey;

            FGPUSpline& Header  = Splines.AddGetRef(Cache.Header);
            Header.PointOffset  = (uint32)Points.Num();
            Header.SampleOffset = (uint32)Samples.Num();
            Points.Append(Cache.Points);
            Samples.Append(Cache.Samples);
        });

        SweepUnseenExtractCache(SplineCache, ExtractSerial, (uint32)Splines.Num());
    }

    void FDefaultSceneRenderer::BuildSplineCache(FSplineExtractCache& Cache, ECS::FEntity Entity, const SSplineComponent& Spline, const FMatrix4& LocalToWorld)
    {
        SplineSampleScratch.Reset();
        const float TotalLength = BuildSplineSamples(Spline, LocalToWorld, SplineSampleScratch);

        FGPUSpline& Header  = Cache.Header;
        Header.LocalToWorld = LocalToWorld;
        Header.WorldToLocal = Math::Inverse(LocalToWorld);
        Header.PointOffset  = 0u;
        Header.PointCount   = (uint32)Spline.Points.Num();
        Header.SampleOffset = 0u;
        Header.SampleCount  = (uint32)SplineSampleScratch.Num();
        Header.TotalLength  = TotalLength;
        Header.Flags        = Spline.bClosedLoop ? SPLINE_FLAG_CLOSED_LOOP : 0u;
        Header.EntityID     = (uint32)Entity.Value;
        Header._Pad         = 0u;

        // Control points go up in WORLD space to match the samples, so no shader needs to know which.
        Cache.Points.Reset();
        for (const SSplinePoint& Point : Spline.Points)
        {
            FGPUSplinePoint& Gpu = Cache.Points.EmplaceGetRef();
            Gpu.Location      = FVector3(LocalToWorld * FVector4(Point.Location, 1.0f));
            Gpu.Roll          = Point.Roll;
            // Tangents are directions, so w = 0 keeps the translation out of them.
            Gpu.ArriveTangent = FVector3(LocalToWorld * FVector4(Point.ArriveTangent, 0.0f));
            Gpu._Pad0         = 0.0f;
            Gpu.LeaveTangent  = FVector3(LocalToWorld * FVector4(Point.LeaveTangent, 0.0f));
            Gpu._Pad1         = 0.0f;
            Gpu.Scale         = Point.Scale;
            Gpu._Pad2         = 0.0f;
        }

        Cache.Samples.Reset();
        for (const FSplineSample& Sample : SplineSampleScratch)
        {
            FGPUSplineSample& Gpu = Cache.Samples.EmplaceGetRef();
            Gpu.Position      = Sample.Position;
            Gpu.DistanceAlong = Sample.DistanceAlong;
            Gpu.Tangent       = Sample.Tangent;
            Gpu.Key           = Sample.Key;
            Gpu.Up            = Sample.Up;
            Gpu.Roll          = Sample.Roll;
            Gpu.Scale         = Sample.Scale;
            Gpu._Pad          = 0.0f;
        }
    }

    FDefaultSceneRenderer::FProbeSortEntry FDefaultSceneRenderer::MakeProbeSortEntry(const SReflectionProbeComponent& Probe, const FMatrix4& WorldMatrix)
    {
        const FVector3 Extent = (Probe.Shape == EReflectionProbeShape::Sphere)
                                    ? FVector3(Math::Max(Probe.Extent.x, 0.001f))
                                    : Math::Max(Probe.Extent, FVector3(0.001f));

        FProbeSortEntry Entry;
        Entry.Gpu.ProbeToWorld    = Math::Scale(WorldMatrix, Extent);
        Entry.Gpu.WorldToProbe    = Math::Inverse(Entry.Gpu.ProbeToWorld);

        const FVector3 CaptureWorld = FVector3(WorldMatrix * FVector4(Probe.CaptureOffset, 1.0f));
        Entry.Gpu.CapturePosition = FVector4(CaptureWorld, 0.0f);
        // The slice, z, is the probe's final position once sorted.
        Entry.Gpu.Params          = FVector4(Math::Max(Probe.Brightness, 0.0f),
                                             Probe.Shape == EReflectionProbeShape::Sphere ? 1.0f : 0.0f,
                                             0.0f,
                                             Math::Max(Probe.BlendDistance, 0.0f));

        Entry.Capture.Position      = CaptureWorld;
        Entry.Capture.NearPlane     = Math::Max(Probe.CaptureNearPlane, 0.001f);
        Entry.Capture.FarPlane      = Math::Max(Probe.CaptureFarPlane, Entry.Capture.NearPlane + 1.0f);
        Entry.Capture.FaceSize      = GetReflectionProbeFaceSize(Probe.Resolution);
        Entry.Capture.bAlwaysUpdate = (Probe.UpdateMode == EReflectionProbeUpdateMode::Always);
        Entry.Capture.bClearToColor = (Probe.ClearMode == EReflectionProbeClearMode::SolidColor);
        Entry.Capture.ClearColor    = Probe.BackgroundColor;

        Entry.Priority = Probe.Priority;
        return Entry;
    }

    bool FDefaultSceneRenderer::GatherReflectionProbes(ECS::FRegistry& Registry)
    {
        using namespace SceneRendererExtractProbesInternal;

        ProbeSortScratch.Reset();

        auto ProbeView = Registry.View<ECS::Read<SReflectionProbeComponent>, ECS::Read<STransformComponent>, ECS::Without<SDisabledTag>>();

        bool  bChanged = false;
        int32 Visited  = 0;
        ProbeView.ForEach([&](ECS::FEntity Entity, const SReflectionProbeComponent& Probe, const STransformComponent& Transform)
        {
            if (!Probe.bEnabled || static_cast<uint32>(Visited) >= MaxReflectionProbes)
            {
                return;
            }

            FProbeExtractCache& Cache = ProbeCache.FindOrAdd(Entity);
            Cache.SeenSerial = ExtractSerial;

            // Computed rather than cached, since probes extract before this frame's transforms resolve.
            const FTransform WorldTransform = Transform.GetWorldTransform();
            const uint64     Key   = ProbeInputKey(Probe, WorldTransform);
            if (!Cache.bValid || Cache.Key != Key)
            {
                Cache.Entry  = MakeProbeSortEntry(Probe, WorldTransform.GetMatrix());
                Cache.Key    = Key;
                Cache.bValid = true;
                bChanged     = true;
            }

            if (Visited < ProbeOrder.Num())
            {
                bChanged |= ProbeOrder[Visited] != Entity;
                ProbeOrder[Visited] = Entity;
            }
            else
            {
                bChanged = true;
                ProbeOrder.Add(Entity);
            }
            ++Visited;

            ProbeSortScratch.Add(Cache.Entry);
        });

        bChanged |= Visited != ProbeOrder.Num();
        ProbeOrder.SetNum(Visited);

        SweepUnseenExtractCache(ProbeCache, ExtractSerial, (uint32)Visited);
        return bChanged;
    }

    void FDefaultSceneRenderer::ExtractReflectionProbes(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        using namespace SceneRendererExtractProbesInternal;

        LUMINA_PROFILE_SECTION("Extract Reflection Probes");

        auto& Probes   = Frame.ReflectionProbes.Probes;
        auto& Captures = Frame.ReflectionProbes.Captures;
        Frame.ReflectionProbes.bNeedsRebake   = false;
        Frame.ReflectionProbes.bLayoutChanged = false;

        // An unchanged set leaves last frame's sorted arrays in place, and those already matched the bake snapshot.
        if (GatherReflectionProbes(Registry))
        {
            Algo::StableSort(ProbeSortScratch, [](const FProbeSortEntry& A, const FProbeSortEntry& B)
            {
                return A.Priority > B.Priority;
            });

            Probes.Reset();
            Captures.Reset();
            Probes.Reserve(ProbeSortScratch.Num());
            Captures.Reserve(ProbeSortScratch.Num());
            for (int32 i = 0; i < ProbeSortScratch.Num(); ++i)
            {
                FGPUReflectionProbe& Gpu = Probes.AddGetRef(ProbeSortScratch[i].Gpu);
                Gpu.Params.z = (float)i;
                Captures.Add(ProbeSortScratch[i].Capture);
            }

            DiffReflectionProbesAgainstBake(Frame);
        }

        const uint32 RebakeRequests = GReflectionProbeRebakeRequests.load(std::memory_order_relaxed);
        if (RebakeRequests != LastSeenRebakeRequest)
        {
            LastSeenRebakeRequest = RebakeRequests;
            Frame.ReflectionProbes.bNeedsRebake = true;
        }

        BakedProbeMask |= CompletedProbeBakes.exchange(0, std::memory_order_acq_rel);

        if (Frame.ReflectionProbes.bNeedsRebake)
        {
            if (Frame.ReflectionProbes.bLayoutChanged)
            {
                BakedProbeMask = 0;
            }

            PendingProbeBakes.Reset();
            for (int32 i = 0; i < Probes.Num(); ++i)
            {
                PendingProbeBakes.Add(i);
            }
        }

        for (int32 i = 0; i < Probes.Num(); ++i)
        {
            Probes[i].CapturePosition.w = ((BakedProbeMask >> i) & 1u) ? 1.0f : 0.0f;
        }

        ScheduleReflectionProbeBake(Frame);
    }

    void FDefaultSceneRenderer::DiffReflectionProbesAgainstBake(FFrameData& Frame)
    {
        using namespace SceneRendererExtractProbesInternal;

        constexpr float ProbeEpsilon = 1e-4f;

        const auto& Probes   = Frame.ReflectionProbes.Probes;
        const auto& Captures = Frame.ReflectionProbes.Captures;

        const bool bLayoutChanged = (Probes.Num() != LastExtractedProbes.Num());

        bool bContentChanged = bLayoutChanged;
        for (int32 i = 0; i < Probes.Num() && !bContentChanged; ++i)
        {
            bContentChanged =
                   !MatrixNearlyEqual(Probes[i].WorldToProbe, LastExtractedProbes[i].WorldToProbe, ProbeEpsilon)
                || Math::Abs(Probes[i].CapturePosition.x - LastExtractedProbes[i].CapturePosition.x) > ProbeEpsilon
                || Math::Abs(Probes[i].CapturePosition.y - LastExtractedProbes[i].CapturePosition.y) > ProbeEpsilon
                || Math::Abs(Probes[i].CapturePosition.z - LastExtractedProbes[i].CapturePosition.z) > ProbeEpsilon
                || (Captures[i].FaceSize  != LastExtractedCaptures[i].FaceSize)
                || (Captures[i].NearPlane != LastExtractedCaptures[i].NearPlane)
                || (Captures[i].FarPlane  != LastExtractedCaptures[i].FarPlane);
        }

        if (bContentChanged)
        {
            Frame.ReflectionProbes.bNeedsRebake = true;
            LastExtractedProbes   = Probes;
            LastExtractedCaptures = Captures;
        }
        Frame.ReflectionProbes.bLayoutChanged = bLayoutChanged;
    }

    void FDefaultSceneRenderer::ScheduleReflectionProbeBake(FFrameData& Frame)
    {
        auto& Bake = Frame.ReflectionProbes;
        Bake.BakingProbe   = -1;
        Bake.BakeViewIndex = -1;

        const uint32 NumProbes = (uint32)Bake.Captures.Num();

        uint32 ProbeIndex = Constants::kIndexNoneU32;
        bool   bFromQueue = false;

        while (!PendingProbeBakes.IsEmpty())
        {
            const uint32 Queued = PendingProbeBakes[0];
            if (Queued < NumProbes)
            {
                ProbeIndex = Queued;
                bFromQueue = true;
                break;
            }
            // The set shrank out from under a queued index; drop it.
            PendingProbeBakes.RemoveAt(0);
        }

        if (ProbeIndex == Constants::kIndexNoneU32)
        {
            for (uint32 Step = 0; Step < NumProbes; ++Step)
            {
                const uint32 Candidate = (AlwaysProbeCursor + Step) % NumProbes;
                if (Bake.Captures[Candidate].bAlwaysUpdate)
                {
                    ProbeIndex        = Candidate;
                    AlwaysProbeCursor = (Candidate + 1) % NumProbes;
                    break;
                }
            }
        }

        if (ProbeIndex == Constants::kIndexNoneU32)
        {
            return;
        }

        const FReflectionProbeCapture& Capture = Bake.Captures[ProbeIndex];

        if (ProbeBakeViewIndex >= 0 && ProbeBakeViewSize != Capture.FaceSize)
        {
            // Tier changed. Release the reservation so the wrong-sized view returns to the pool.
            if (ProbeBakeViewIndex < SceneViews.Num())
            {
                SceneViews[ProbeBakeViewIndex].bReservedForProbeBake = false;
            }
            ProbeBakeViewIndex = -1;
        }

        if (ProbeBakeViewIndex < 0)
        {
            const int32 ViewIndex = RegisterCaptureView(FUIntVector2(Capture.FaceSize, Capture.FaceSize));
            if (ViewIndex < 0)
            {
                // Out of view slots; leave the probe queued rather than dropping it.
                return;
            }
            SceneViews[ViewIndex].bReservedForProbeBake = true;
            ProbeBakeViewIndex = ViewIndex;
            ProbeBakeViewSize  = Capture.FaceSize;
        }

        const int32 ViewIndex = ProbeBakeViewIndex;

        static const FVector3 FaceForward[6] = {
            FVector3( 1.0f,  0.0f,  0.0f), FVector3(-1.0f,  0.0f,  0.0f),
            FVector3( 0.0f,  1.0f,  0.0f), FVector3( 0.0f, -1.0f,  0.0f),
            FVector3( 0.0f,  0.0f,  1.0f), FVector3( 0.0f,  0.0f, -1.0f),
        };
        static const FVector3 FaceUp[6] = {
            FVector3( 0.0f,  1.0f,  0.0f), FVector3( 0.0f,  1.0f,  0.0f),
            FVector3( 0.0f,  0.0f, -1.0f), FVector3( 0.0f,  0.0f,  1.0f),
            FVector3( 0.0f,  1.0f,  0.0f), FVector3( 0.0f,  1.0f,  0.0f),
        };

        for (int32 Face = 0; Face < 6; ++Face)
        {
            // 90 degrees at aspect 1 is what makes six frusta tile the sphere exactly with no seam.
            FViewVolume& Volume = Bake.FaceVolumes[Face];
            Volume = FViewVolume(90.0f, 1.0f, Capture.NearPlane, Capture.FarPlane);
            Volume.SetView(Capture.Position, FaceForward[Face], FaceUp[Face]);
            // BuildCullViews fills this in.
            Bake.FaceCullViews[Face] = Constants::kIndexNoneU32;
        }

        Bake.BakingProbe   = (int32)ProbeIndex;
        Bake.BakeViewIndex = ViewIndex;
        Bake.BakeFaceSize  = Capture.FaceSize;
        if (bFromQueue)
        {
            PendingProbeBakes.RemoveAt(0);
        }
    }
}
