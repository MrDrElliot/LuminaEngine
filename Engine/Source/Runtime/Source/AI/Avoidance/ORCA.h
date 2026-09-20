#pragma once

#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"

// Optimal Reciprocal Collision Avoidance in the XZ plane, free of ECS, world and allocation.

namespace Lumina::Avoidance
{
    // Velocities v satisfying Det(Direction, v - Point) >= 0 are permitted by this constraint.
    struct FORCALine
    {
        float PointX = 0.0f;
        float PointZ = 0.0f;
        float DirX   = 0.0f;
        float DirZ   = 0.0f;
    };

    // Bounds every fixed-size buffer in the solver, so a caller's neighbor cap must not exceed it.
    inline constexpr int32 kMaxORCALines = 16;

    struct FORCAAgent
    {
        float PosX     = 0.0f;
        float PosZ     = 0.0f;
        float VelX     = 0.0f;
        float VelZ     = 0.0f;
        float Radius   = 0.5f;
        float MaxSpeed = 1.0f;
    };

    namespace Detail
    {
        FORCEINLINE float Det(float AX, float AZ, float BX, float BZ) { return AX * BZ - AZ * BX; }

        inline constexpr float kEpsilon = 1e-5f;
    }

    // Responsibility is Self's share of the correction, so 0.5 splits it evenly between the two agents.
    inline void BuildAgentLine(const FORCAAgent& Self, const FORCAAgent& Other, float InvTimeHorizon,
                               float InvTimeStep, float Responsibility, FORCALine& Out)
    {
        const float RelPosX = Other.PosX - Self.PosX;
        const float RelPosZ = Other.PosZ - Self.PosZ;
        const float RelVelX = Self.VelX - Other.VelX;
        const float RelVelZ = Self.VelZ - Other.VelZ;

        const float DistSq         = RelPosX * RelPosX + RelPosZ * RelPosZ;
        const float CombinedRadius = Self.Radius + Other.Radius;
        const float RadiusSq       = CombinedRadius * CombinedRadius;

        float UX, UZ, DirX, DirZ;

        if (DistSq > RadiusSq)
        {
            // From the cutoff circle's center to the relative velocity.
            const float WX = RelVelX - InvTimeHorizon * RelPosX;
            const float WZ = RelVelZ - InvTimeHorizon * RelPosZ;

            const float WLenSq = WX * WX + WZ * WZ;
            const float Dot    = WX * RelPosX + WZ * RelPosZ;

            if (Dot < 0.0f && Dot * Dot > RadiusSq * WLenSq)
            {
                const float WLen  = Math::Sqrt(WLenSq);
                const float InvW  = WLen > Detail::kEpsilon ? 1.0f / WLen : 0.0f;
                const float UnitX = WX * InvW;
                const float UnitZ = WZ * InvW;

                DirX = UnitZ;
                DirZ = -UnitX;

                const float Scale = CombinedRadius * InvTimeHorizon - WLen;
                UX = Scale * UnitX;
                UZ = Scale * UnitZ;
            }
            else
            {
                // Past the cutoff arc the nearest boundary is one of the velocity obstacle's two legs.
                const float Leg     = Math::Sqrt(Math::Max(DistSq - RadiusSq, 0.0f));
                const float InvDist = 1.0f / DistSq;

                if (Detail::Det(RelPosX, RelPosZ, WX, WZ) > 0.0f)
                {
                    DirX = (RelPosX * Leg - RelPosZ * CombinedRadius) * InvDist;
                    DirZ = (RelPosX * CombinedRadius + RelPosZ * Leg) * InvDist;
                }
                else
                {
                    DirX = -(RelPosX * Leg + RelPosZ * CombinedRadius) * InvDist;
                    DirZ = -(-RelPosX * CombinedRadius + RelPosZ * Leg) * InvDist;
                }

                const float DotLeg = RelVelX * DirX + RelVelZ * DirZ;
                UX = DotLeg * DirX - RelVelX;
                UZ = DotLeg * DirZ - RelVelZ;
            }
        }
        else
        {
            // Already overlapping, so the horizon collapses to one step and the pair is pushed apart.
            const float WX = RelVelX - InvTimeStep * RelPosX;
            const float WZ = RelVelZ - InvTimeStep * RelPosZ;

            const float WLen  = Math::Sqrt(WX * WX + WZ * WZ);
            const float InvW  = WLen > Detail::kEpsilon ? 1.0f / WLen : 0.0f;
            const float UnitX = WX * InvW;
            const float UnitZ = WZ * InvW;

            DirX = UnitZ;
            DirZ = -UnitX;

            const float Scale = CombinedRadius * InvTimeStep - WLen;
            UX = Scale * UnitX;
            UZ = Scale * UnitZ;
        }

        Out.PointX = Self.VelX + Responsibility * UX;
        Out.PointZ = Self.VelZ + Responsibility * UZ;
        Out.DirX   = DirX;
        Out.DirZ   = DirZ;
    }

    namespace Detail
    {
        // False when the feasible interval on line LineNo is empty, given lines before it are satisfied.
        inline bool SolveOnLine(const FORCALine* Lines, int32 LineNo, float MaxSpeed, float OptX, float OptZ,
                                bool bDirectionOpt, float& OutX, float& OutZ)
        {
            const FORCALine& L = Lines[LineNo];

            const float Dot     = L.PointX * L.DirX + L.PointZ * L.DirZ;
            const float Discrim = Dot * Dot + MaxSpeed * MaxSpeed - (L.PointX * L.PointX + L.PointZ * L.PointZ);
            if (Discrim < 0.0f)
            {
                return false;
            }

            const float Root   = Math::Sqrt(Discrim);
            float       TLeft  = -Dot - Root;
            float       TRight = -Dot + Root;

            for (int32 i = 0; i < LineNo; ++i)
            {
                const float Denominator = Det(L.DirX, L.DirZ, Lines[i].DirX, Lines[i].DirZ);
                const float Numerator   = Det(Lines[i].DirX, Lines[i].DirZ,
                                              L.PointX - Lines[i].PointX, L.PointZ - Lines[i].PointZ);

                if (Math::Abs(Denominator) <= kEpsilon)
                {
                    // Parallel, so line i either excludes this one outright or constrains nothing on it.
                    if (Numerator < 0.0f)
                    {
                        return false;
                    }
                    continue;
                }

                const float T = Numerator / Denominator;
                if (Denominator >= 0.0f)
                {
                    TRight = Math::Min(TRight, T);
                }
                else
                {
                    TLeft = Math::Max(TLeft, T);
                }

                if (TLeft > TRight)
                {
                    return false;
                }
            }

            float T;
            if (bDirectionOpt)
            {
                T = (OptX * L.DirX + OptZ * L.DirZ) > 0.0f ? TRight : TLeft;
            }
            else
            {
                T = L.DirX * (OptX - L.PointX) + L.DirZ * (OptZ - L.PointZ);
                T = Math::Clamp(T, TLeft, TRight);
            }

            OutX = L.PointX + T * L.DirX;
            OutZ = L.PointZ + T * L.DirZ;
            return true;
        }

        // Index of the first line that could not be satisfied, or NumLines when all of them were.
        inline int32 SolveLines(const FORCALine* Lines, int32 NumLines, float MaxSpeed, float OptX, float OptZ,
                                bool bDirectionOpt, float& OutX, float& OutZ)
        {
            if (bDirectionOpt)
            {
                OutX = OptX * MaxSpeed;
                OutZ = OptZ * MaxSpeed;
            }
            else if (OptX * OptX + OptZ * OptZ > MaxSpeed * MaxSpeed)
            {
                const float Len = Math::Sqrt(OptX * OptX + OptZ * OptZ);
                const float Inv = Len > kEpsilon ? MaxSpeed / Len : 0.0f;
                OutX = OptX * Inv;
                OutZ = OptZ * Inv;
            }
            else
            {
                OutX = OptX;
                OutZ = OptZ;
            }

            for (int32 i = 0; i < NumLines; ++i)
            {
                if (Det(Lines[i].DirX, Lines[i].DirZ, Lines[i].PointX - OutX, Lines[i].PointZ - OutZ) <= 0.0f)
                {
                    continue;
                }

                const float PrevX = OutX;
                const float PrevZ = OutZ;
                if (!SolveOnLine(Lines, i, MaxSpeed, OptX, OptZ, bDirectionOpt, OutX, OutZ))
                {
                    OutX = PrevX;
                    OutZ = PrevZ;
                    return i;
                }
            }

            return NumLines;
        }

        // Dense-crowd fallback that minimizes the worst violation rather than freezing the agent.
        inline void SolveRelaxed(const FORCALine* Lines, int32 NumLines, int32 BeginLine, float MaxSpeed,
                                 float& OutX, float& OutZ)
        {
            float Distance = 0.0f;

            for (int32 i = BeginLine; i < NumLines; ++i)
            {
                if (Det(Lines[i].DirX, Lines[i].DirZ, Lines[i].PointX - OutX, Lines[i].PointZ - OutZ) <= Distance)
                {
                    continue;
                }

                FORCALine Projected[kMaxORCALines];
                int32     NumProjected = 0;

                for (int32 j = 0; j < i && NumProjected < kMaxORCALines; ++j)
                {
                    const float Determinant = Det(Lines[i].DirX, Lines[i].DirZ, Lines[j].DirX, Lines[j].DirZ);

                    FORCALine Line;
                    if (Math::Abs(Determinant) <= kEpsilon)
                    {
                        if (Lines[i].DirX * Lines[j].DirX + Lines[i].DirZ * Lines[j].DirZ > 0.0f)
                        {
                            continue;
                        }
                        Line.PointX = 0.5f * (Lines[i].PointX + Lines[j].PointX);
                        Line.PointZ = 0.5f * (Lines[i].PointZ + Lines[j].PointZ);
                    }
                    else
                    {
                        const float T = Det(Lines[j].DirX, Lines[j].DirZ,
                                            Lines[i].PointX - Lines[j].PointX,
                                            Lines[i].PointZ - Lines[j].PointZ) / Determinant;
                        Line.PointX = Lines[i].PointX + T * Lines[i].DirX;
                        Line.PointZ = Lines[i].PointZ + T * Lines[i].DirZ;
                    }

                    const float DX  = Lines[j].DirX - Lines[i].DirX;
                    const float DZ  = Lines[j].DirZ - Lines[i].DirZ;
                    const float Len = Math::Sqrt(DX * DX + DZ * DZ);
                    if (Len <= kEpsilon)
                    {
                        continue;
                    }
                    Line.DirX = DX / Len;
                    Line.DirZ = DZ / Len;

                    Projected[NumProjected++] = Line;
                }

                const float PrevX = OutX;
                const float PrevZ = OutZ;
                if (SolveLines(Projected, NumProjected, MaxSpeed, -Lines[i].DirZ, Lines[i].DirX, true, OutX, OutZ)
                    < NumProjected)
                {
                    OutX = PrevX;
                    OutZ = PrevZ;
                }

                Distance = Det(Lines[i].DirX, Lines[i].DirZ, Lines[i].PointX - OutX, Lines[i].PointZ - OutZ);
            }
        }
    }

    // A symmetric standoff yields a pure deceleration constraint, so identical agents stall touching.
    inline void ApplySymmetryBreak(uint32 Seed, float Magnitude, float& PrefX, float& PrefZ)
    {
        if (Magnitude <= 0.0f)
        {
            return;
        }

        uint32 Hash = Seed * 2654435761u;
        Hash ^= Hash >> 15;
        Hash *= 2246822519u;
        Hash ^= Hash >> 13;

        const float Angle = (float)(Hash & 0xFFFFu) * (6.28318531f / 65536.0f);
        PrefX += Math::Cos(Angle) * Magnitude;
        PrefZ += Math::Sin(Angle) * Magnitude;
    }

    // Velocity nearest the preferred one that satisfies every constraint and stays within MaxSpeed.
    inline void SolveVelocity(const FORCALine* Lines, int32 NumLines, float MaxSpeed,
                              float PrefVelX, float PrefVelZ, float& OutX, float& OutZ)
    {
        const int32 Failed = Detail::SolveLines(Lines, NumLines, MaxSpeed, PrefVelX, PrefVelZ, false, OutX, OutZ);
        if (Failed < NumLines)
        {
            Detail::SolveRelaxed(Lines, NumLines, Failed, MaxSpeed, OutX, OutZ);
        }
    }
}
