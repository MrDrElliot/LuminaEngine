#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "BlendSpace.h"

#include "Assets/AssetTypes/Mesh/Animation/Animation.h"

namespace Lumina
{
    namespace
    {
        // In-circle test, orientation independent so the caller does not have to wind its triangles.
        bool CircumcircleContains(const FVector2& A, const FVector2& B, const FVector2& C, const FVector2& P)
        {
            const float ADX = A.x - P.x, ADY = A.y - P.y;
            const float BDX = B.x - P.x, BDY = B.y - P.y;
            const float CDX = C.x - P.x, CDY = C.y - P.y;

            const float Determinant =
                  (ADX * ADX + ADY * ADY) * (BDX * CDY - CDX * BDY)
                - (BDX * BDX + BDY * BDY) * (ADX * CDY - CDX * ADY)
                + (CDX * CDX + CDY * CDY) * (ADX * BDY - BDX * ADY);

            const float SignedArea = (B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y);
            return SignedArea > 0.0f ? Determinant > 0.0f : Determinant < 0.0f;
        }

        // Barycentric coordinates of P in triangle ABC. False when the triangle is degenerate.
        bool Barycentric(const FVector2& A, const FVector2& B, const FVector2& C, const FVector2& P,
                         float& OutU, float& OutV, float& OutW)
        {
            const FVector2 V0 = B - A;
            const FVector2 V1 = C - A;
            const FVector2 V2 = P - A;

            const float Denominator = V0.x * V1.y - V1.x * V0.y;
            if (Math::Abs(Denominator) < 1e-8f)
            {
                return false;
            }

            OutV = (V2.x * V1.y - V1.x * V2.y) / Denominator;
            OutW = (V0.x * V2.y - V2.x * V0.y) / Denominator;
            OutU = 1.0f - OutV - OutW;
            return true;
        }
    }

    float SBlendSpaceAxis::Normalize(float Value) const
    {
        const float Range = Max - Min;
        if (Math::Abs(Range) < 1e-6f)
        {
            return 0.0f;
        }

        return Math::Clamp((Value - Min) / Range, 0.0f, 1.0f);
    }

    void SBlendSpaceAxis::SnapSmoothing(float Target, float& OutValue, float& OutVelocity) const
    {
        OutValue = Target;
        OutVelocity = 0.0f;
    }

    float SBlendSpaceAxis::Smooth(float Target, float DeltaTime, float& InOutValue, float& InOutVelocity) const
    {
        if (!IsSmoothed() || DeltaTime <= 0.0f)
        {
            SnapSmoothing(Target, InOutValue, InOutVelocity);
            return InOutValue;
        }

        // Chasing a target off the end of the axis would spend the whole smoothing time out of sight.
        const float Low  = Math::Min(Min, Max);
        const float High = Math::Max(Min, Max);
        Target = Math::Clamp(Target, Low, High);

        const float Previous = InOutValue;

        switch (Smoothing)
        {
        case EBlendSpaceSmoothing::Linear:
            {
                const float Step = ((High - Low) / SmoothingTime) * DeltaTime;
                const float Remaining = Target - Previous;
                InOutValue = Math::Abs(Remaining) <= Step ? Target : Previous + Math::Sign(Remaining) * Step;
                break;
            }

        case EBlendSpaceSmoothing::ExponentialDecay:
            {
                // Three decay constants leaves 5% of the gap after SmoothingTime, matching how the other modes read.
                InOutValue = Previous + (Target - Previous) * (1.0f - Math::Exp(-DeltaTime * 3.0f / SmoothingTime));
                break;
            }

        case EBlendSpaceSmoothing::SpringDamper:
            {
                // Implicit Euler, so a long frame decays toward the target instead of exploding away from it.
                const float Omega = 4.75f / SmoothingTime;
                const float Zeta  = Math::Max(DampingRatio, 0.0f);
                const float F     = 1.0f + 2.0f * DeltaTime * Zeta * Omega;
                const float HHOO  = DeltaTime * DeltaTime * Omega * Omega;

                InOutValue = (F * Previous + DeltaTime * InOutVelocity + HHOO * Target) / (F + HHOO);
                break;
            }

        default:
            InOutValue = Target;
            break;
        }

        if (MaxSpeed > 0.0f)
        {
            const float Step = MaxSpeed * DeltaTime;
            InOutValue = Math::Clamp(InOutValue, Previous - Step, Previous + Step);
        }

        InOutValue = Math::Clamp(InOutValue, Low, High);

        // Implicit Euler puts the new velocity exactly here, so the clamps above stay consistent with it.
        InOutVelocity = (InOutValue - Previous) / DeltaTime;
        return InOutValue;
    }

    void FBlendSpaceWeights::Add(int32 SampleIndex, float Weight)
    {
        if (Count >= MaxContributions || Weight <= 1e-5f || SampleIndex == INDEX_NONE)
        {
            return;
        }

        SampleIndices[Count] = SampleIndex;
        Weights[Count] = Weight;
        ++Count;
    }

    void CBlendSpace::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Animation");
        CObject::PostLoad();
        RebuildTopology();
    }

    void CBlendSpace::RebuildTopology()
    {
        Triangles.clear();
        SortedOrder.clear();

        const int32 NumSamples = (int32)Samples.size();
        TopologySampleCount = NumSamples;

        if (NumSamples == 0)
        {
            return;
        }

        SortedOrder.reserve(NumSamples);
        for (int32 i = 0; i < NumSamples; ++i)
        {
            SortedOrder.push_back(i);
        }

        Algo::Sort(SortedOrder, [this](int32 A, int32 B)
        {
            return Samples[A].Position.x < Samples[B].Position.x;
        });

        if (AxisCount != EBlendSpaceAxes::Two || NumSamples < 3)
        {
            return;
        }

        // Normalized so both axes carry equal weight, or the triangulation degenerates into slivers.
        TVector<FVector2> Points;
        Points.reserve(NumSamples + 3);
        for (const SBlendSpaceSample& Sample : Samples)
        {
            Points.push_back(FVector2(AxisX.Normalize(Sample.Position.x), AxisY.Normalize(Sample.Position.y)));
        }

        // Far enough out that no super vertex lands in a real circumcircle and skips a hull triangle.
        const int32 Super0 = NumSamples;
        Points.push_back(FVector2(-1000.0f, -1000.0f));
        Points.push_back(FVector2( 2001.0f, -1000.0f));
        Points.push_back(FVector2(-1000.0f,  2001.0f));

        TVector<FBlendSpaceTriangle> Working;
        Working.push_back({ Super0, Super0 + 1, Super0 + 2 });

        TVector<FBlendSpaceTriangle> Bad;
        TVector<TPair<int32, int32>> Edges;

        for (int32 PointIndex = 0; PointIndex < NumSamples; ++PointIndex)
        {
            const FVector2& P = Points[PointIndex];

            Bad.clear();
            for (int32 i = (int32)Working.size() - 1; i >= 0; --i)
            {
                const FBlendSpaceTriangle& Tri = Working[i];
                if (CircumcircleContains(Points[Tri.A], Points[Tri.B], Points[Tri.C], P))
                {
                    Bad.push_back(Tri);
                    Working.erase(Working.begin() + i);
                }
            }

            // The hole's boundary is every edge belonging to exactly one removed triangle.
            Edges.clear();
            for (const FBlendSpaceTriangle& Tri : Bad)
            {
                const TPair<int32, int32> TriEdges[3] =
                {
                    { Math::Min(Tri.A, Tri.B), Math::Max(Tri.A, Tri.B) },
                    { Math::Min(Tri.B, Tri.C), Math::Max(Tri.B, Tri.C) },
                    { Math::Min(Tri.C, Tri.A), Math::Max(Tri.C, Tri.A) },
                };

                for (const TPair<int32, int32>& Edge : TriEdges)
                {
                    bool bShared = false;
                    for (int32 e = (int32)Edges.size() - 1; e >= 0; --e)
                    {
                        if (Edges[e] == Edge)
                        {
                            Edges.erase(Edges.begin() + e);
                            bShared = true;
                            break;
                        }
                    }

                    if (!bShared)
                    {
                        Edges.push_back(Edge);
                    }
                }
            }

            for (const TPair<int32, int32>& Edge : Edges)
            {
                Working.push_back({ Edge.first, Edge.second, PointIndex });
            }
        }

        for (const FBlendSpaceTriangle& Tri : Working)
        {
            if (Tri.A < NumSamples && Tri.B < NumSamples && Tri.C < NumSamples)
            {
                Triangles.push_back(Tri);
            }
        }
    }

    void CBlendSpace::Evaluate(const FVector2& Input, FBlendSpaceWeights& OutWeights) const
    {
        OutWeights = FBlendSpaceWeights();

        if (Samples.empty())
        {
            return;
        }

        if (Samples.size() == 1)
        {
            OutWeights.Add(0, 1.0f);
            return;
        }

        // Stale derived data indexes off the end of Samples rather than merely blending wrong.
        if (!HasValidTopology())
        {
            OutWeights.Add(0, 1.0f);
            return;
        }

        if (AxisCount == EBlendSpaceAxes::One)
        {
            EvaluateOneAxis(Input.x, OutWeights);
        }
        else
        {
            EvaluateTwoAxis(Input, OutWeights);
        }
    }

    void CBlendSpace::EvaluateOneAxis(float X, FBlendSpaceWeights& OutWeights) const
    {
        // SortedOrder ascends in X, so the bracketing pair is the first sample at or past the input.
        const int32 Count = (int32)SortedOrder.size();

        if (X <= Samples[SortedOrder[0]].Position.x)
        {
            OutWeights.Add(SortedOrder[0], 1.0f);
            return;
        }

        if (X >= Samples[SortedOrder[Count - 1]].Position.x)
        {
            OutWeights.Add(SortedOrder[Count - 1], 1.0f);
            return;
        }

        for (int32 i = 1; i < Count; ++i)
        {
            const int32 LowIndex = SortedOrder[i - 1];
            const int32 HighIndex = SortedOrder[i];

            const float Low = Samples[LowIndex].Position.x;
            const float High = Samples[HighIndex].Position.x;

            if (X > High)
            {
                continue;
            }

            const float Span = High - Low;
            const float Alpha = Span > 1e-6f ? Math::Clamp((X - Low) / Span, 0.0f, 1.0f) : 0.0f;

            OutWeights.Add(LowIndex, 1.0f - Alpha);
            OutWeights.Add(HighIndex, Alpha);
            return;
        }

        OutWeights.Add(SortedOrder[Count - 1], 1.0f);
    }

    void CBlendSpace::EvaluateTwoAxis(const FVector2& Input, FBlendSpaceWeights& OutWeights) const
    {
        const FVector2 P(AxisX.Normalize(Input.x), AxisY.Normalize(Input.y));

        // Falls back to the nearest pair projected onto their segment, which is what a 1D space does.
        if (Triangles.empty())
        {
            int32 NearestA = INDEX_NONE;
            int32 NearestB = INDEX_NONE;
            float BestA = 1e30f;
            float BestB = 1e30f;

            for (int32 i = 0; i < (int32)Samples.size(); ++i)
            {
                const FVector2 Q(AxisX.Normalize(Samples[i].Position.x), AxisY.Normalize(Samples[i].Position.y));
                const float DistanceSq = Math::LengthSquared(Q - P);

                if (DistanceSq < BestA)
                {
                    BestB = BestA;
                    NearestB = NearestA;
                    BestA = DistanceSq;
                    NearestA = i;
                }
                else if (DistanceSq < BestB)
                {
                    BestB = DistanceSq;
                    NearestB = i;
                }
            }

            if (NearestB == INDEX_NONE)
            {
                OutWeights.Add(NearestA, 1.0f);
                return;
            }

            const FVector2 A(AxisX.Normalize(Samples[NearestA].Position.x), AxisY.Normalize(Samples[NearestA].Position.y));
            const FVector2 B(AxisX.Normalize(Samples[NearestB].Position.x), AxisY.Normalize(Samples[NearestB].Position.y));

            const FVector2 Segment = B - A;
            const float SegmentLengthSq = Math::LengthSquared(Segment);
            const float Alpha = SegmentLengthSq > 1e-8f
                ? Math::Clamp(Math::Dot(P - A, Segment) / SegmentLengthSq, 0.0f, 1.0f)
                : 0.0f;

            OutWeights.Add(NearestA, 1.0f - Alpha);
            OutWeights.Add(NearestB, Alpha);
            return;
        }

        // Clamping and renormalizing slides along the nearest edge instead of snapping between triangles.
        int32 BestTriangle = INDEX_NONE;
        float BestScore = -1e30f;
        float BestU = 0.0f, BestV = 0.0f, BestW = 0.0f;

        for (int32 i = 0; i < (int32)Triangles.size(); ++i)
        {
            const FBlendSpaceTriangle& Tri = Triangles[i];

            const FVector2 A(AxisX.Normalize(Samples[Tri.A].Position.x), AxisY.Normalize(Samples[Tri.A].Position.y));
            const FVector2 B(AxisX.Normalize(Samples[Tri.B].Position.x), AxisY.Normalize(Samples[Tri.B].Position.y));
            const FVector2 C(AxisX.Normalize(Samples[Tri.C].Position.x), AxisY.Normalize(Samples[Tri.C].Position.y));

            float U, V, W;
            if (!Barycentric(A, B, C, P, U, V, W))
            {
                continue;
            }

            const float Score = Math::Min(U, Math::Min(V, W));
            if (Score > BestScore)
            {
                BestScore = Score;
                BestTriangle = i;
                BestU = U;
                BestV = V;
                BestW = W;
            }

            if (Score >= 0.0f)
            {
                break;
            }
        }

        if (BestTriangle == INDEX_NONE)
        {
            OutWeights.Add(0, 1.0f);
            return;
        }

        BestU = Math::Max(BestU, 0.0f);
        BestV = Math::Max(BestV, 0.0f);
        BestW = Math::Max(BestW, 0.0f);

        const float Total = BestU + BestV + BestW;
        if (Total < 1e-6f)
        {
            OutWeights.Add(Triangles[BestTriangle].A, 1.0f);
            return;
        }

        const FBlendSpaceTriangle& Tri = Triangles[BestTriangle];
        OutWeights.Add(Tri.A, BestU / Total);
        OutWeights.Add(Tri.B, BestV / Total);
        OutWeights.Add(Tri.C, BestW / Total);
    }

    float CBlendSpace::GetBlendedDuration(const FBlendSpaceWeights& Weights) const
    {
        float Duration = 0.0f;
        float TotalWeight = 0.0f;

        for (int32 i = 0; i < Weights.Count; ++i)
        {
            const int32 SampleIndex = Weights.SampleIndices[i];
            if (SampleIndex < 0 || SampleIndex >= (int32)Samples.size())
            {
                continue;
            }

            const CAnimation* Animation = Samples[SampleIndex].Animation.Get();
            if (Animation == nullptr)
            {
                continue;
            }

            Duration += Animation->GetDuration() * Weights.Weights[i];
            TotalWeight += Weights.Weights[i];
        }

        return TotalWeight > 1e-5f ? Duration / TotalWeight : 0.0f;
    }
}
