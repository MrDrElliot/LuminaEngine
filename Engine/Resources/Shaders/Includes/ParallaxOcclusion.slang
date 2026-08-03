#pragma once
#include "Includes/SceneGlobals.slang"

// Parallax Occlusion Mapping (Tatarchuk, "Practical Parallax Occlusion Mapping for Highly Detailed
// Surface Rendering", SIGGRAPH 2006). Per-pixel ray tracing of a height field in tangent space: the
// height map is treated as a surface pushed DOWN from the polygon (white = top of the field = the
// polygon plane, black = deepest), and each pixel walks the view ray until it crosses that surface.
// The resulting UV offset is fed to every other texture fetch, so albedo/normal/roughness all shift
// together and the surface gains real motion parallax and self-occlusion.
//
// What makes this the POM paper and not plain relief mapping is the intersection: after the linear
// search finds the crossing step, the height field is treated as piecewise LINEAR over that step and
// the ray/segment intersection is solved analytically. Relief mapping's piecewise-CONSTANT sampling
// plus a binary search resolves the crossing to only ~8 bits, which stair-steps at oblique angles and
// is why those implementations need a depth bias that flattens everything toward the horizon.
//
// Consumed by the material graph's ParallaxOcclusionMapping node, which runs in the pixel stage of any
// $MATERIAL_INPUTS template and reads the graph local contract (UV0, WorldNormal, WorldTangent,
// WorldPosition, UV0_DDX, UV0_DDY).
//
// Not handled, by construction: silhouettes stay flat (the technique displaces texture coordinates,
// not geometry) and depth is not modified, so debug/scene geometry intersects the reference plane
// rather than the displaced surface. Both are inherent to POM; the paper's own answer to the first is
// to author art that hides it.

// Vertex tangent frame, byte-identical to what GetWorldNormal builds (SurfaceShading.slang calls this).
// Both MUST agree: POM offsets UVs along this basis and the normal map is decoded through it, so any
// disagreement shears the parallax against the lighting. Rows are (T, B, N), so mul(v, Frame) takes a
// tangent-space vector to world and mul(Frame, v) takes a world-space vector to tangent space.
//
// bDegenerate reports a tangent that vanished under Gram-Schmidt (untangented mesh, or a tangent
// parallel to the normal). The frame returned in that case is still orthonormal -- built from an
// arbitrary axis -- but it carries no relation to the UV layout, so callers that need a MEANINGFUL
// tangent direction (normal decode, POM) must treat it as "no tangent frame" rather than use it.
float3x3 BuildTangentFrame(float3 FragNormal, float4 FragTangent, out bool bDegenerate)
{
    // N is assumed unit-length: every caller passes the already-normalized interpolated normal.
    float3 N = FragNormal;

    float3 T = FragTangent.xyz - N * dot(FragTangent.xyz, N);
    float TLen2 = dot(T, T);

    bDegenerate = TLen2 < 1e-8;
    if (bDegenerate)
    {
        float3 Up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
        T = normalize(cross(Up, N));
    }
    else
    {
        T *= rsqrt(TLen2);
    }

    float3 B = cross(N, T) * FragTangent.w;
    return float3x3(T, B, N);
}

float3x3 BuildTangentFrame(float3 FragNormal, float4 FragTangent)
{
    bool bDegenerate;
    return BuildTangentFrame(FragNormal, FragTangent, bDegenerate);
}

// Mip level this pixel would sample the height field at, from the UV gradients and the texture size.
// The standard isotropic formula the hardware uses: log2 of the longer texel-space gradient.
float ParallaxMipLevel(uint HeightTextureIndex, float2 UVDdx, float2 UVDdy)
{
    float2 TexSize;
    gTextures2D[HeightTextureIndex].GetDimensions(TexSize.x, TexSize.y);

    float2 Dx = UVDdx * TexSize;
    float2 Dy = UVDdy * TexSize;
    return 0.5 * log2(max(max(dot(Dx, Dx), dot(Dy, Dy)), 1e-12));
}

// Height at a UV, sampled at a fixed mip. Fixed rather than gradient-based on purpose: the march takes
// up to MaxSamples fetches, and SampleGrad per iteration costs both rate and registers for a mip that
// does not meaningfully change over the offset. Red channel; height maps cook to RGBA8 grayscale.
float SampleParallaxHeight(uint HeightTextureIndex, uint SamplerIndex, float2 UV, float Mip)
{
    return SampleTexture2DLevel(HeightTextureIndex, SamplerIndex, UV, Mip).r;
}

struct FParallaxResult
{
    float2 UV;       // displaced texture coordinate; feed this to every other sample
    float  Height;   // height field value at the intersection, 0 (deep) .. 1 (surface)
    float  Weight;   // 1 = full parallax, 0 = fully faded to flat normal mapping at distance
};

// Height field ray-trace. ViewWS is the normalized surface->camera vector.
//
// MinSamples/MaxSamples bracket the dynamic sampling rate: the paper scales it linearly with the angle
// between the geometric normal and the view ray, n = nmin + (N.V)(nmax - nmin), so grazing angles --
// where apparent parallax is largest and a coarse march misses features -- get the most samples. Note
// the paper's expression as written gives the MAXIMUM at N.V == 1 (head-on); what actually matters for
// quality is the opposite, so this uses lerp(Max, Min, N.V): most samples at grazing, fewest head-on.
//
// HeightScale is the depth of the height field in UV-relative units (the artist-facing "how deep").
// LODThreshold is the mip level at which POM has faded entirely to normal mapping; the fade occupies
// the one mip below it.
FParallaxResult ParallaxOcclusion(
    uint   HeightTextureIndex,
    uint   SamplerIndex,
    float2 UV,
    float3 ViewWS,
    float3 NormalWS,
    float4 TangentWS,
    float  HeightScale,
    float  MinSamples,
    float  MaxSamples,
    float  LODThreshold,
    float2 UVDdx,
    float2 UVDdy)
{
    FParallaxResult Result;
    Result.UV     = UV;
    Result.Height = 1.0;
    Result.Weight = 0.0;

    // Global quality scaling (r.POM.*), applied on top of the material's authored values so one CVar
    // dials every POM material without recompiling any of them. SampleScale 0 disables the feature.
    float SampleScale = Scene().ParallaxSettings.SampleScale;
    if (SampleScale <= 0.0)
    {
        return Result;
    }
    MinSamples   = max(MinSamples * SampleScale, 1.0);
    MaxSamples   = max(MaxSamples * SampleScale, MinSamples);
    LODThreshold = LODThreshold + Scene().ParallaxSettings.LODBias;

    // Distance fade. Above the threshold mip the height field is smaller than the pixel footprint, so
    // the march can only produce noise -- the paper renders those levels as plain normal mapping. The
    // fade blends the OFFSET toward zero across the last mip rather than blending two shaded results:
    // a zero offset IS normal mapping, so the two endpoints match exactly for a fraction of the cost.
    float Mip    = ParallaxMipLevel(HeightTextureIndex, UVDdx, UVDdy);
    float Weight = saturate(LODThreshold - Mip);
    if (Weight <= 0.0)
    {
        return Result;
    }
    Result.Weight = Weight;

    bool     bDegenerate;
    float3x3 Frame  = BuildTangentFrame(NormalWS, TangentWS, bDegenerate);
    float3   ViewTS = mul(Frame, ViewWS);

    // No tangent frame tied to the UV layout (untangented mesh), or a ray parallel to the surface (the
    // offset diverges). Neither can produce a meaningful march; fall back to flat.
    if (bDegenerate || abs(ViewTS.z) < 1e-4)
    {
        Result.Weight = 0.0;
        return Result;
    }

    // Maximum UV displacement, reached when the ray descends the full height of the field. Dividing the
    // lateral component by z is the perspective term -- it is what makes the offset grow at grazing
    // angles instead of the fixed-length offset plain parallax mapping uses.
    float2 MaxOffset = (-ViewTS.xy / ViewTS.z) * HeightScale;

    // Dynamic sampling rate, from the GEOMETRIC normal (not the mapped one -- the mapped normal is what
    // we are on our way to computing).
    float  NdotV    = saturate(dot(NormalWS, ViewWS));
    int    NumSteps = (int)max(lerp(MaxSamples, MinSamples, NdotV), 1.0);
    float  StepSize = 1.0 / (float)NumSteps;
    float2 StepUV   = MaxOffset * StepSize;

    float  RayHeight      = 1.0;                 // ray marches down from the polygon plane
    float2 CurrentOffset  = float2(0.0, 0.0);
    float2 PreviousOffset = float2(0.0, 0.0);
    float  CurrentHeight  = 1.0;
    float  PreviousHeight = 1.0;

    // Linear search with dynamic early-out. Only regular (non-dependent) fetches along a straight UV
    // line, so this stays texture-cache coherent -- the reason the paper drops the binary search.
    int Step = 0;
    while (Step < NumSteps)
    {
        CurrentHeight = SampleParallaxHeight(HeightTextureIndex, SamplerIndex, UV + CurrentOffset, Mip);

        if (CurrentHeight > RayHeight)
        {
            // Crossed the surface between the previous and current sample. Solve the intersection of
            // the view ray with the LINE SEGMENT joining the two height samples, instead of accepting
            // the sample point itself. Delta1 is how far the surface is above the ray now; Delta2 how
            // far the ray was above the surface at the previous step. The ratio splits the step at the
            // true crossing -- this is the paper's high-precision intersection, and it is what removes
            // the stair-stepping without any depth bias.
            float Delta1 = CurrentHeight - RayHeight;
            float Delta2 = (RayHeight + StepSize) - PreviousHeight;
            float Ratio  = Delta1 / max(Delta1 + Delta2, 1e-6);

            CurrentOffset = lerp(CurrentOffset, PreviousOffset, Ratio);
            CurrentHeight = lerp(CurrentHeight, PreviousHeight, Ratio);
            break;
        }

        Step++;
        RayHeight     -= StepSize;
        PreviousOffset = CurrentOffset;
        PreviousHeight = CurrentHeight;
        CurrentOffset += StepUV;
    }

    Result.UV     = UV + CurrentOffset * Weight;
    Result.Height = CurrentHeight;
    return Result;
}

// Soft self-shadowing of the height field. From the displaced point, march toward the light and collect
// blockers; the paper's insight is to keep sampling PAST the first blocker and weight each sample by
// its distance from the reference point, which approximates the penumbra (wp = ws(dr - db)/db -- a
// blocker close to the receiving surface casts a tight shadow, a distant one a wide soft edge). Simply
// stopping at the first blocker, as relief mapping does, gives hard aliased shadow edges.
//
// One light only: this runs in the material graph, before the clustered light loop, so it is evaluated
// against the directional light (the sun). Returns 1 = fully lit.
float ParallaxSelfShadow(
    uint   HeightTextureIndex,
    uint   SamplerIndex,
    float2 DisplacedUV,
    float  SurfaceHeight,
    float3 LightWS,
    float3 NormalWS,
    float4 TangentWS,
    float  HeightScale,
    float  NumSamples,
    float  Softness,
    float  Mip)
{
    // Surfaces facing away from the light are already fully shadowed by N.L; tracing them is wasted
    // work and would double-darken the terminator. The paper gates on exactly this test.
    float NdotL = dot(NormalWS, LightWS);
    if (NdotL <= 0.0)
    {
        return 1.0;
    }

    float3x3 Frame   = BuildTangentFrame(NormalWS, TangentWS);
    float3   LightTS = mul(Frame, LightWS);
    if (LightTS.z < 1e-4)
    {
        return 1.0;
    }

    float ShadowScale = Scene().ParallaxSettings.ShadowSampleScale;
    if (ShadowScale <= 0.0)
    {
        return 1.0;
    }

    int Steps = (int)max(NumSamples * ShadowScale, 1.0);

    // The light ray climbs from the intersection height back to the polygon plane; walk that span.
    float2 MaxOffset = (LightTS.xy / LightTS.z) * HeightScale * (1.0 - SurfaceHeight);
    float  StepSize  = 1.0 / (float)Steps;

    float Occlusion = 0.0;
    for (int i = 1; i <= Steps; ++i)
    {
        float  T      = (float)i * StepSize;
        float2 Offset = MaxOffset * T;
        float  Height = SampleParallaxHeight(HeightTextureIndex, SamplerIndex, DisplacedUV + Offset, Mip);

        // Height of the light ray at this step, and how far the height field pokes above it.
        float RayHeight = SurfaceHeight + (1.0 - SurfaceHeight) * T;
        float Blocked   = max(Height - RayHeight, 0.0);

        // Distance weighting: a near blocker contributes a sharp, strong occlusion, a far one a weak
        // diffuse contribution. Taking the max rather than a sum keeps a single tall blocker from
        // being out-voted by a run of shallow ones.
        Occlusion = max(Occlusion, Blocked * (1.0 - T));
    }

    return saturate(1.0 - Occlusion * Softness);
}
