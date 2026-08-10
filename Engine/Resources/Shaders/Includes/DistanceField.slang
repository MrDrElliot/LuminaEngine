#pragma once
#include "Includes/Common.slang"
#include "Includes/GlobalRHI.slang"

// Per-mesh signed distance field, baked once per MESH in LOCAL space so scaled instances reuse it.
// A narrow band: outside +/-MaxDistance it saturates, and every consumer treats that as no information.

static const uint kInvalidDistanceField = 0xFFFFFFFFu;

// Mirror of Lumina::EDistanceFieldFlags.
static const uint kDistanceFieldTwoSided = 1u;

struct FDistanceFieldVolume
{
    uint   TextureIndex;
    bool   bTwoSided;
    float3 VolumeMin;
    float3 VolumeSize;
    float  MaxDistance;

    bool IsValid() { return TextureIndex != kInvalidDistanceField; }

    // Local position -> [0,1] volume coordinate. Hardware linear filtering samples at texel centres,
    // which is exactly where the voxel centres were evaluated, so this needs no half-texel correction.
    float3 LocalToUVW(float3 LocalPosition)
    {
        return (LocalPosition - VolumeMin) / max(VolumeSize, float3(1e-6));
    }

    // True when the local point lies inside the baked volume. Outside it the texture clamps, which
    // extrudes the boundary voxels outward -- readable, but not a distance.
    bool ContainsLocal(float3 LocalPosition)
    {
        float3 UVW = LocalToUVW(LocalPosition);
        return all(UVW >= 0.0) && all(UVW <= 1.0);
    }
};

FDistanceFieldVolume GetDistanceFieldVolume(FMeshletHeader* Header)
{
    FDistanceFieldVolume V;
    V.TextureIndex = Header->DistanceFieldIndex;
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

// Signed distance at a LOCAL-space point, in local units. Points outside the volume clamp to the
// boundary value; callers that care use ContainsLocal to tell the two apart.
float SampleDistanceFieldLocal(FDistanceFieldVolume Volume, float3 LocalPosition)
{
    if (!Volume.IsValid())
    {
        return Volume.MaxDistance;
    }

    float Encoded = gTextures3D[Volume.TextureIndex]
        .SampleLevel(gSamplers[SAMPLER_LINEAR_CLAMP], Volume.LocalToUVW(LocalPosition), 0.0).r;

    // Two-sided fields spend the whole byte on [0, Max]; signed ones split it around 0.5.
    return Volume.bTwoSided
        ? Encoded * Volume.MaxDistance
        : (Encoded * 2.0 - 1.0) * Volume.MaxDistance;
}

// Central difference over one voxel step, normalized -- the outward surface normal. Returns 0 on a
// saturated plateau, where the difference degenerates; callers must check for that.
float3 SampleDistanceFieldGradientLocal(FDistanceFieldVolume Volume, float3 LocalPosition)
{
    if (!Volume.IsValid())
    {
        return float3(0.0, 0.0, 0.0);
    }

    // One step per axis in local units. Tied to the volume extent rather than a constant so a
    // centimetre-scale prop and a hundred-metre one both differentiate over a comparable voxel count.
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

// Per-axis world scale of an instance transform: the length of each basis column.
float3 GetInstanceScale(float4x4 ModelMatrix)
{
    return float3(
        length(mul(ModelMatrix, float4(1.0, 0.0, 0.0, 0.0)).xyz),
        length(mul(ModelMatrix, float4(0.0, 1.0, 0.0, 0.0)).xyz),
        length(mul(ModelMatrix, float4(0.0, 0.0, 1.0, 0.0)).xyz));
}

// Exact inverse of an always-affine object->world. Cofactor form rather than a general 4x4 inverse:
// cheaper, and finite for a degenerate scale. M[i] is a mathematical ROW (see FTransform3x4).
float4x4 MakeInstanceWorldToLocal(float4x4 ModelMatrix)
{
    // CalculateNormalMatrix returns (A^-1)^T for the upper-left 3x3 A, so its transpose is A^-1.
    float3x3 InvRotScale = transpose(CalculateNormalMatrix(ModelMatrix));

    float3 Translation = float3(ModelMatrix[0].w, ModelMatrix[1].w, ModelMatrix[2].w);
    float3 InvTranslation = -mul(InvRotScale, Translation);

    return float4x4(
        float4(InvRotScale[0], InvTranslation.x),
        float4(InvRotScale[1], InvTranslation.y),
        float4(InvRotScale[2], InvTranslation.z),
        float4(0.0, 0.0, 0.0, 1.0));
}

// The MINIMUM axis scale, not the average: only the minimum is guaranteed not to overstate free
// space under non-uniform scale, and overstating it is what makes a sphere trace tunnel.
float LocalDistanceToWorld(float LocalDistance, float3 InstanceScale)
{
    return LocalDistance * min(InstanceScale.x, min(InstanceScale.y, InstanceScale.z));
}

struct FDistanceFieldQuery
{
    float  Distance;    // world units, signed (negative = inside). MaxDistance when invalid.
    float3 Gradient;    // world-space outward normal of the field, 0 when undefined
    bool   bValid;      // a field exists AND the point is inside its volume
};

// WorldToLocal is passed in because a material graph already has the instance loaded and the inverse
// is worth computing once per pixel rather than once per sample.
FDistanceFieldQuery QueryDistanceField(FDistanceFieldVolume Volume, float4x4 WorldToLocal,
                                       float3 InstanceScale, float3 WorldPosition)
{
    FDistanceFieldQuery Q;
    Q.Distance = 0.0;
    Q.Gradient = float3(0.0, 0.0, 0.0);
    Q.bValid   = false;

    if (!Volume.IsValid())
    {
        // A large positive value, not zero: "no field" must read as "nothing nearby", and zero would
        // read as "exactly on a surface" to every consumer downstream.
        Q.Distance = 1e6;
        return Q;
    }

    float3 LocalPosition = mul(WorldToLocal, float4(WorldPosition, 1.0)).xyz;

    Q.bValid   = Volume.ContainsLocal(LocalPosition);
    Q.Distance = LocalDistanceToWorld(SampleDistanceFieldLocal(Volume, LocalPosition), InstanceScale);

    // The gradient is a surface normal, so it transforms by the inverse-transpose of object->world --
    // which, since WorldToLocal already IS the inverse, is just its transpose (M[i] is a row).
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

// SDF cone-trace occlusion (Quilez): one march covers a whole cone, far cheaper than a hemisphere.
// Self-occlusion only -- contact darkening in a mesh's own creases, and it follows deformation.
float DistanceFieldConeOcclusion(FDistanceFieldVolume Volume, float3 LocalPosition, float3 LocalDirection,
                                 float MaxLocalDistance, float ConeAngle, int StepCount)
{
    if (!Volume.IsValid())
    {
        return 1.0;
    }

    float Visibility = 1.0;

    // Start off the surface. The field is zero AT the surface, so a march that begins there reports
    // full occlusion on the first sample and never recovers.
    float MinStep = MaxLocalDistance / (float)max(StepCount, 1);
    float T       = MinStep;

    for (int i = 0; i < StepCount; ++i)
    {
        float3 P = LocalPosition + LocalDirection * T;
        if (!Volume.ContainsLocal(P))
        {
            // Left the volume: no more of this mesh to occlude with, so the rest of the cone is open. Breaking
            // rather than clamping keeps a small prop from shadowing itself with the plateau outside its bounds.
            break;
        }

        float D = SampleDistanceFieldLocal(Volume, P);

        // Only reachable when the start point sits under geometry. Treat as fully occluded rather than
        // letting the negative value invert the ratio below.
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

// Thickness below a surface point, by marching INTO the mesh and measuring how far the walk stays
// inside. Feeds subsurface: thin geometry transmits. Returns 0 for a two-sided field.
float DistanceFieldThickness(FDistanceFieldVolume Volume, float3 LocalPosition, float3 LocalNormal,
                             float MaxLocalDistance, float3 InstanceScale, int StepCount)
{
    if (!Volume.IsValid() || Volume.bTwoSided)
    {
        return 0.0;
    }

    float StepSize  = MaxLocalDistance / (float)max(StepCount, 1);
    float Thickness = 0.0;

    // Fixed-step, not sphere-traced: the walk is INSIDE, where a sphere trace would step by the distance
    // to the nearest wall and skip straight past the thin section being measured.
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
            // Exited out the far side; T is the crossing, one step of quantisation.
            Thickness = T;
            break;
        }

        Thickness = T;
    }

    return LocalDistanceToWorld(Thickness, InstanceScale);
}

// Sphere trace against the field. Returns the world distance to the hit, or -1 on a miss. Local-space
// march (the field's native space), converted on the way out.
float DistanceFieldTrace(FDistanceFieldVolume Volume, float3 LocalOrigin, float3 LocalDirection,
                         float MaxLocalDistance, float3 InstanceScale, int StepCount)
{
    if (!Volume.IsValid())
    {
        return -1.0;
    }

    // Hit threshold as a fraction of the band rather than an absolute: an absolute epsilon that works
    // on a metre-scale prop makes a kilometre-scale one march forever.
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
