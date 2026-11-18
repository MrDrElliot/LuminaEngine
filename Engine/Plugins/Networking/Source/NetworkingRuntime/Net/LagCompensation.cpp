#include "RuntimePCH.h"
#include "LagCompensation.h"

#include "Containers/Algorithm.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    namespace LagCompensationInternal
    {
        bool EntityLess(const FLagCompensationSample& A, const FLagCompensationSample& B)
        {
            return A.Entity.Value < B.Entity.Value;
        }

        // Ericson's closest points between two segments, as fractions along each.
        void ClosestSegmentSegment(const FVector3& P1, const FVector3& Q1, const FVector3& P2, const FVector3& Q2, float& S, float& T)
        {
            constexpr float kEpsilon = 1e-8f;
            const FVector3 D1 = Q1 - P1;
            const FVector3 D2 = Q2 - P2;
            const FVector3 R  = P1 - P2;
            const float A = Math::Dot(D1, D1);
            const float E = Math::Dot(D2, D2);
            const float F = Math::Dot(D2, R);

            if (A <= kEpsilon && E <= kEpsilon)
            {
                S = T = 0.0f;
                return;
            }
            if (A <= kEpsilon)
            {
                S = 0.0f;
                T = Math::Clamp(F / E, 0.0f, 1.0f);
                return;
            }

            const float C = Math::Dot(D1, R);
            if (E <= kEpsilon)
            {
                T = 0.0f;
                S = Math::Clamp(-C / A, 0.0f, 1.0f);
                return;
            }

            const float B = Math::Dot(D1, D2);
            const float Denom = A * E - B * B;
            S = Denom != 0.0f ? Math::Clamp((B * F - C * E) / Denom, 0.0f, 1.0f) : 0.0f;
            T = (B * S + F) / E;
            if (T < 0.0f)
            {
                T = 0.0f;
                S = Math::Clamp(-C / A, 0.0f, 1.0f);
            }
            else if (T > 1.0f)
            {
                T = 1.0f;
                S = Math::Clamp((B - C) / A, 0.0f, 1.0f);
            }
        }

        FVector3 ClosestOnSegment(const FVector3& A, const FVector3& B, const FVector3& Point)
        {
            const FVector3 AB = B - A;
            const float LengthSq = Math::Dot(AB, AB);
            const float T = LengthSq > 0.0f ? Math::Clamp(Math::Dot(Point - A, AB) / LengthSq, 0.0f, 1.0f) : 0.0f;
            return A + AB * T;
        }

        // Visits every hitbox at ViewTime that Near keeps. Near sees the older frame's sample and how far any hitbox moved since, so only survivors are blended.
        template<typename TNear, typename TVisit>
        void ForEachHitboxAt(const FLagCompensationHistory& History, double ViewTime, TNear&& Near, TVisit&& Visit)
        {
            if (History.Count == 0)
            {
                return;
            }

            ViewTime = History.ClampViewTime(ViewTime);
            int32 OlderAge = History.Count - 1;
            for (int32 Age = 0; Age < History.Count; ++Age)
            {
                if (History.FrameAt(Age).Time <= ViewTime)
                {
                    OlderAge = Age;
                    break;
                }
            }

            const FLagCompensationFrame& Older = History.FrameAt(OlderAge);
            const bool bBlend = OlderAge > 0 && Older.Time < ViewTime;
            const FLagCompensationFrame* Newer = bBlend ? &History.FrameAt(OlderAge - 1) : nullptr;
            const float Inflate = Newer != nullptr ? Newer->MaxMoveFromPrevious : 0.0f;
            const float Alpha = Newer != nullptr ? (float)((ViewTime - Older.Time) / Math::Max(Newer->Time - Older.Time, 1e-9)) : 0.0f;

            for (const FLagCompensationSample& Then : Older.Samples)
            {
                if (!Near(Then, Inflate))
                {
                    continue;
                }

                // A hitbox in only one of the two frames was spawned or removed between them, so it is taken as it was.
                const FLagCompensationSample* Next = Newer != nullptr ? Newer->Find(Then.Entity) : nullptr;
                if (Next == nullptr)
                {
                    Visit(Then);
                    continue;
                }

                FLagCompensationSample Blended;
                Blended.Entity      = Then.Entity;
                Blended.A           = Then.A + (Next->A - Then.A) * Alpha;
                Blended.B           = Then.B + (Next->B - Then.B) * Alpha;
                Blended.Radius      = Then.Radius + (Next->Radius - Then.Radius) * Alpha;
                Blended.BoundRadius = Math::Max(Then.BoundRadius, Next->BoundRadius);
                Visit(Blended);
            }
        }

        // The sphere around a hitbox, grown by how far anything moved since it was recorded, touches a sphere of Reach around Point.
        bool BoundTouches(const FLagCompensationSample& Sample, float Inflate, const FVector3& Point, float Reach)
        {
            const FVector3 Middle = (Sample.A + Sample.B) * 0.5f;
            const float Bound = Sample.BoundRadius + Inflate + Reach;
            return Math::Dot(Middle - Point, Middle - Point) <= Bound * Bound;
        }
    }

    void FLagCompensationHistory::EnsureCapacity(double Time)
    {
        // Sized from the observed tick spacing, so a faster server keeps more frames for the same window.
        int32 Wanted = 32;
        if (Count > 0)
        {
            const double Spacing = Math::Max(Time - GetNewestTime(), 1.0 / 240.0);
            Wanted = Math::Clamp((int32)(MaxRewindSeconds / Spacing) + 3, 8, 512);
        }
        if (Frames.Num() >= Wanted)
        {
            return;
        }

        // Grown by rebuilding oldest first, which keeps every frame's position in the ring consistent.
        TVector<FLagCompensationFrame> Grown;
        Grown.SetNum(Wanted);
        for (int32 Age = Count - 1, Index = 0; Age >= 0; --Age, ++Index)
        {
            Grown[Index] = Move(Frames[(Newest - Age + Frames.Num()) % Frames.Num()]);
        }
        Newest = Count - 1;
        Frames = Move(Grown);
    }

    FLagCompensationFrame& FLagCompensationHistory::BeginRecord(double Time)
    {
        EnsureCapacity(Time);
        Newest = (Newest + 1) % Frames.Num();
        Count = Math::Min(Count + 1, Frames.Num());

        FLagCompensationFrame& Frame = Frames[Newest];
        Frame.Time = Time;
        Frame.Samples.Reset();
        return Frame;
    }

    void FLagCompensationHistory::FinishRecord()
    {
        TVector<FLagCompensationSample>& Samples = Frames[Newest].Samples;
        Algo::Sort(Samples, &LagCompensationInternal::EntityLess);
        for (FLagCompensationSample& Sample : Samples)
        {
            Sample.BoundRadius = 0.5f * Math::Sqrt(Math::Dot(Sample.B - Sample.A, Sample.B - Sample.A)) + Sample.Radius;
        }

        // Once per tick, so a shot can reject a hitbox from the older frame alone and blend only what is near it.
        float MaxMove = 0.0f;
        if (Count >= 2)
        {
            const FLagCompensationFrame& Previous = FrameAt(1);
            int32 P = 0;
            for (const FLagCompensationSample& Sample : Samples)
            {
                while (P < Previous.Samples.Num() && Previous.Samples[P].Entity.Value < Sample.Entity.Value)
                {
                    ++P;
                }
                if (P < Previous.Samples.Num() && Previous.Samples[P].Entity == Sample.Entity)
                {
                    const FLagCompensationSample& Then = Previous.Samples[P];
                    const float MoveA = Math::Dot(Sample.A - Then.A, Sample.A - Then.A);
                    const float MoveB = Math::Dot(Sample.B - Then.B, Sample.B - Then.B);
                    MaxMove = Math::Max(MaxMove, Math::Sqrt(Math::Max(MoveA, MoveB)) + Math::Abs(Sample.BoundRadius - Then.BoundRadius));
                }
            }
        }
        Frames[Newest].MaxMoveFromPrevious = MaxMove;

        // One frame past the window stays, so the oldest allowed time still has a frame on each side.
        while (Count > 2 && FrameAt(Count - 2).Time < GetNewestTime() - MaxRewindSeconds)
        {
            --Count;
        }
    }

    const FLagCompensationSample* FLagCompensationFrame::Find(ECS::FEntity Entity) const
    {
        const FLagCompensationSample* Found = Algo::LowerBound(Samples, Entity, [](const FLagCompensationSample& Sample, ECS::FEntity Key)
        {
            return Sample.Entity.Value < Key.Value;
        });
        return Found != Samples.end() && Found->Entity == Entity ? Found : nullptr;
    }

    void FLagCompensationHistory::Record(double Time, TSpan<const FLagCompensationSample> Samples)
    {
        FLagCompensationFrame& Frame = BeginRecord(Time);
        Frame.Samples.Append(Samples.GetData(), (int32)Samples.Num());
        FinishRecord();
    }

    double FLagCompensationHistory::GetNewestTime() const
    {
        return Count > 0 ? Frames[Newest].Time : 0.0;
    }

    double FLagCompensationHistory::GetOldestTime() const
    {
        return Count > 0 ? FrameAt(Count - 1).Time : 0.0;
    }

    double FLagCompensationHistory::ClampViewTime(double ViewTime) const
    {
        if (Count == 0)
        {
            return ViewTime;
        }
        const double Earliest = Math::Max(GetOldestTime(), GetNewestTime() - MaxRewindSeconds);
        return Math::Clamp(ViewTime, Earliest, GetNewestTime());
    }

    const FLagCompensationFrame& FLagCompensationHistory::FrameAt(int32 Age) const
    {
        return Frames[(Newest - Age + Frames.Num()) % Frames.Num()];
    }

    SLagCompensatedHit LagCompensation::Raycast(const FLagCompensationHistory& History, double ViewTime, const FVector3& Origin,
        const FVector3& Direction, float MaxDistance, float Thickness, ECS::FEntity Ignore)
    {
        using namespace LagCompensationInternal;

        SLagCompensatedHit Best;
        const float DirLengthSq = Math::Dot(Direction, Direction);
        if (DirLengthSq <= 0.0f || MaxDistance <= 0.0f)
        {
            return Best;
        }
        const FVector3 Dir = Direction * (1.0f / Math::Sqrt(DirLengthSq));
        const FVector3 End = Origin + Dir * MaxDistance;
        Best.Distance = MaxDistance;

        // A sphere around each capsule rejects nearly every hitbox for a handful of multiplies, before any blend or exact test.
        const auto NearRay = [&](const FLagCompensationSample& Hitbox, float Inflate)
        {
            if (Hitbox.Entity == Ignore)
            {
                return false;
            }
            const FVector3 Middle = (Hitbox.A + Hitbox.B) * 0.5f;
            const float Along = Math::Clamp(Math::Dot(Middle - Origin, Dir), 0.0f, MaxDistance);
            return BoundTouches(Hitbox, Inflate, Origin + Dir * Along, Thickness);
        };

        ForEachHitboxAt(History, ViewTime, NearRay, [&](const FLagCompensationSample& Hitbox)
        {

            float S = 0.0f;
            float T = 0.0f;
            ClosestSegmentSegment(Origin, End, Hitbox.A, Hitbox.B, S, T);
            const FVector3 OnRay = Origin + Dir * (S * MaxDistance);
            const FVector3 OnBox = Hitbox.A + (Hitbox.B - Hitbox.A) * T;
            const float Reach = Hitbox.Radius + Thickness;
            const float DistSq = Math::Dot(OnRay - OnBox, OnRay - OnBox);
            if (DistSq > Reach * Reach)
            {
                return;
            }

            // Backed off from the closest approach to where the ray enters the capsule's rounded surface there.
            const float Entry = Math::Max(S * MaxDistance - Math::Sqrt(Reach * Reach - DistSq), 0.0f);
            if (Entry >= Best.Distance && Best.bHit)
            {
                return;
            }

            Best.bHit     = true;
            Best.Entity   = Hitbox.Entity;
            Best.Distance = Entry;
            Best.Point    = Origin + Dir * Entry;
            const FVector3 Outward = Best.Point - ClosestOnSegment(Hitbox.A, Hitbox.B, Best.Point);
            const float OutwardLengthSq = Math::Dot(Outward, Outward);
            Best.Normal   = OutwardLengthSq > 1e-12f ? Outward * (1.0f / Math::Sqrt(OutwardLengthSq)) : -Dir;
        });

        if (!Best.bHit)
        {
            Best.Distance = 0.0f;
        }
        return Best;
    }

    void LagCompensation::OverlapSphere(const FLagCompensationHistory& History, double ViewTime, const FVector3& Center, float Radius,
        TVector<ECS::FEntity>& Out)
    {
        using namespace LagCompensationInternal;

        const auto NearCenter = [&](const FLagCompensationSample& Hitbox, float Inflate) { return BoundTouches(Hitbox, Inflate, Center, Radius); };
        ForEachHitboxAt(History, ViewTime, NearCenter, [&](const FLagCompensationSample& Hitbox)
        {
            const FVector3 Closest = ClosestOnSegment(Hitbox.A, Hitbox.B, Center);
            const float Reach = Hitbox.Radius + Radius;
            if (Math::Dot(Closest - Center, Closest - Center) <= Reach * Reach)
            {
                Out.Add(Hitbox.Entity);
            }
        });
    }
}
