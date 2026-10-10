#pragma once
#include "Includes/Common.slang"
#include "Includes/GlobalRHI.slang"

// Baked once per mesh in local space, and values saturate past +/-MaxDistance, meaning no information.

// Mirror of Lumina::EDistanceFieldFlags.
static const uint kDistanceFieldTwoSided = 1u;

struct FDistanceFieldVolume
{
    FTexture3D Texture;
    bool       bTwoSided;
    float3     VolumeMin;
    float3     VolumeSize;
    float      MaxDistance;

    bool IsValid() { return Texture.IsValid(); }

    // Linear filtering samples at texel centers, where voxels were evaluated, so no half-texel correction.
    float3 LocalToUVW(float3 LocalPosition)
    {
        return (LocalPosition - VolumeMin) / max(VolumeSize, float3(1e-6));
    }

    // Outside the volume the texture clamps, which extrudes boundary voxels and is not a distance.
    bool ContainsLocal(float3 LocalPosition)
    {
        float3 UVW = LocalToUVW(LocalPosition);
        return all(UVW >= 0.0) && all(UVW <= 1.0);
    }
};

FDistanceFieldVolume GetDistanceFieldVolume(FMeshletHeader* Header)
{
    FDistanceFieldVolume V;
    V.Texture      = Header->DistanceField;
    V.bTwoSided    = (Header->DistanceFieldFlags & kDistanceFieldTwoSided) != 0u;
    V.VolumeMin    = Header->DistanceFieldMin;
    V.VolumeSize   = Header->DistanceFieldSize;
    V.MaxDistance  = Header->DistanceFieldMaxDistance;
    return V;
}

FDistanceFieldVolume GetInstanceDistanceFieldVolume(FGPUInstance Instance)
{
    return GetDistanceFieldVolume(Instance.MeshletHeader);
}

// Local units, and points outside the volume clamp to the boundary value.
float SampleDistanceFieldLocal(FDistanceFieldVolume Volume, float3 LocalPosition)
{
    if (!Volume.IsValid())
    {
        return Volume.MaxDistance;
    }

    float Encoded = Volume.Texture
        .SampleLevel(Samplers::LinearClamp, Volume.LocalToUVW(LocalPosition), 0.0).r;

    // Two-sided fields spend the whole byte on [0, Max]; signed ones split it around 0.5.
    return Volume.bTwoSided
        ? Encoded * Volume.MaxDistance
        : (Encoded * 2.0 - 1.0) * Volume.MaxDistance;
}

// Returns zero on a saturated plateau where the difference degenerates.
float3 SampleDistanceFieldGradientLocal(FDistanceFieldVolume Volume, float3 LocalPosition)
{
    if (!Volume.IsValid())
    {
        return float3(0.0, 0.0, 0.0);
    }

    // Tied to the volume extent so meshes of any scale differentiate over a comparable voxel count.
    float3 Step = Volume.VolumeSize * (1.0 / 64.0);

    float3 G;
    G.x = SampleDistanceFieldLocal(Volume, LocalPosition + float3(Step.x, 0, 0))
        - SampleDistanceFieldLocal(Volume, LocalPosition - float3(Step.x, 0, 0));
    G.y = SampleDistanceFieldLocal(Volume, LocalPosition + float3(0, Step.y, 0))
        - SampleDistanceFieldLocal(Volume, LocalPosition - float3(0, Step.y, 0));
    G.z = SampleDistanceFieldLocal(Volume, LocalPosition + float3(0, 0, Step.z))
        - SampleDistanceFieldLocal(Volume, LocalPosition - float3(0, 0, Step.z));

    float Len = length(G);
    return Len > 1e-8 ? G / Len : float3(0.0, 0.0, 0.0);
}

// Minimum axis scale, the only one that never overstates free space under non-uniform scale.
float LocalDistanceToWorld(float LocalDistance, float3 InstanceScale)
{
    return LocalDistance * min(InstanceScale.x, min(InstanceScale.y, InstanceScale.z));
}

struct FDistanceFieldQuery
{
    float  Distance;    // world units, negative inside
    float3 Gradient;    // world-space outward normal of the field, 0 when undefined
    bool   bValid;      // a field exists and the point is inside its volume
};

// Takes WorldToLocal so the inverse is computed once per pixel rather than per sample.
FDistanceFieldQuery QueryDistanceField(FDistanceFieldVolume Volume, float4x4 WorldToLocal,
                                       float3 InstanceScale, float3 WorldPosition)
{
    FDistanceFieldQuery Q;
    Q.Distance = 0.0;
    Q.Gradient = float3(0.0, 0.0, 0.0);
    Q.bValid   = false;

    if (!Volume.IsValid())
    {
        // Large rather than zero, since zero would read as exactly on a surface.
        Q.Distance = 1e6;
        return Q;
    }

    float3 LocalPosition = mul(WorldToLocal, float4(WorldPosition, 1.0)).xyz;

    Q.bValid   = Volume.ContainsLocal(LocalPosition);
    Q.Distance = LocalDistanceToWorld(SampleDistanceFieldLocal(Volume, LocalPosition), InstanceScale);

    // Normals transform by the inverse-transpose, and WorldToLocal is already the inverse.
    float3 LocalGradient = SampleDistanceFieldGradientLocal(Volume, LocalPosition);
    if (any(LocalGradient != 0.0))
    {
        float3 WorldGradient = float3(
            dot(float3(WorldToLocal[0][0], WorldToLocal[1][0], WorldToLocal[2][0]), LocalGradient),
            dot(float3(WorldToLocal[0][1], WorldToLocal[1][1], WorldToLocal[2][1]), LocalGradient),
            dot(float3(WorldToLocal[0][2], WorldToLocal[1][2], WorldToLocal[2][2]), LocalGradient));

        float Len = length(WorldGradient);
        Q.Gradient = Len > 1e-8 ? WorldGradient / Len : float3(0.0, 0.0, 0.0);
    }

    return Q;
}

// SDF cone-trace occlusion (Quilez), self-occlusion only so it follows deformation.
float DistanceFieldConeOcclusion(FDistanceFieldVolume Volume, float3 LocalPosition, float3 LocalDirection,
                                 float MaxLocalDistance, float ConeAngle, int StepCount)
{
    if (!Volume.IsValid())
    {
        return 1.0;
    }

    float Visibility = 1.0;

    // Start off the surface, or the first sample reports full occlusion and never recovers.
    float MinStep = MaxLocalDistance / (float)max(StepCount, 1);
    float T       = MinStep;

    for (int i = 0; i < StepCount; ++i)
    {
        float3 P = LocalPosition + LocalDirection * T;
        if (!Volume.ContainsLocal(P))
        {
            // The rest of the cone is open, so a small prop does not shadow itself with the plateau outside.
            break;
        }

        float D = SampleDistanceFieldLocal(Volume, P);

        // Only reachable when the start point sits under geometry, and a negative D would invert the ratio.
        if (D < 0.0)
        {
            return 0.0;
        }

        Visibility = min(Visibility, D / max(T * ConeAngle, 1e-6));
        T += max(D, MinStep);

        if (T >= MaxLocalDistance)
        {
            break;
        }
    }

    return saturate(Visibility);
}

// Marches into the mesh to measure thickness for subsurface, and returns 0 for a two-sided field.
float DistanceFieldThickness(FDistanceFieldVolume Volume, float3 LocalPosition, float3 LocalNormal,
                             float MaxLocalDistance, float3 InstanceScale, int StepCount)
{
    if (!Volume.IsValid() || Volume.bTwoSided)
    {
        return 0.0;
    }

    float StepSize  = MaxLocalDistance / (float)max(StepCount, 1);
    float Thickness = 0.0;

    // Fixed-step because a sphere trace inside would skip past the thin section being measured.
    for (int i = 1; i <= StepCount; ++i)
    {
        float  T = (float)i * StepSize;
        float3 P = LocalPosition - LocalNormal * T;

        if (!Volume.ContainsLocal(P))
        {
            break;
        }

        if (SampleDistanceFieldLocal(Volume, P) >= 0.0)
        {
            // Exited the far side, within one step of quantization.
            Thickness = T;
            break;
        }

        Thickness = T;
    }

    return LocalDistanceToWorld(Thickness, InstanceScale);
}

// Returns the world distance to the hit, or -1 on a miss.
float DistanceFieldTrace(FDistanceFieldVolume Volume, float3 LocalOrigin, float3 LocalDirection,
                         float MaxLocalDistance, float3 InstanceScale, int StepCount)
{
    if (!Volume.IsValid())
    {
        return -1.0;
    }

    // Relative to the band, since an absolute epsilon makes a kilometer-scale mesh march forever.
    float HitEpsilon = Volume.MaxDistance * 0.01;
    float T          = 0.0;

    for (int i = 0; i < StepCount; ++i)
    {
        float3 P = LocalOrigin + LocalDirection * T;
        if (!Volume.ContainsLocal(P))
        {
            return -1.0;
        }

        float D = SampleDistanceFieldLocal(Volume, P);
        if (D < HitEpsilon)
        {
            return LocalDistanceToWorld(T, InstanceScale);
        }

        T += D;
        if (T > MaxLocalDistance)
        {
            return -1.0;
        }
    }

    return -1.0;
}
