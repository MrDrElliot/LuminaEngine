#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

// Traces each half-resolution texel through the closest-depth pyramid and stores what it hit, weighted by how much the hit can be trusted.

struct FSSRTraceArgs
{
    FTexture2D   Pyramid;     // closest-depth pyramid, mip 0 is the half-resolution depth
    FTexture2D   Surface;     // world normal and roughness of the pixel each pyramid texel kept
    FTexture2D   SceneColor;  // HDR, read while nothing in this pass writes it
    FRWTexture2D OutColor;      // tonemapped radiance times validity, validity in w

    FRWTexture2D OutMip;        // which level of the filtered chain the resolve should read
    uint         TraceW;
    uint         TraceH;
    uint         MipCount;

    uint  MaxIterations;
    float DepthTolerance;   // view-space distance behind a surface a ray may pass and still count as hitting it
    float FadeIn;           // exponent on the screen distance a ray covers, so contact reflections ease in
    float FadeOut;          // exponent on the remaining distance, so rays nearing a screen's width ease out

    float      MaxRoughness;     // rougher surfaces are left to the prefiltered environment
    FTexture2D Depth;       // full-resolution depth, to find the pixel each pyramid texel kept
    uint       ScreenW;
    uint       ScreenH;

    uint  Scale;            // screen pixels per trace texel along each axis
    uint  _Pad0;
    uint  _Pad1;
    uint  _Pad2;
};
FSSRTraceArgs* Pass() { return GetPassArgs<FSSRTraceArgs>(); }

static const float kPi = 3.14159265359;

static const float kMinMipLevel = 1.0;

// Nudges a cell boundary just past itself, so the next step lands in the neighbor rather than on the edge.
static const float kCellEpsilon = 0.000001;

float SSR_PyramidDepth(int2 Cell, int Level)
{
    return Pass().Pyramid.Load(int3(Cell, Level)).r;
}

float3 SSR_ViewPos(float3 ScreenPos)
{
    return ReconstructViewPos(ScreenPos.xy, ScreenPos.z, GetInverseCameraProjection());
}

float3 SSR_ScreenPos(float3 ViewPos)
{
    float4 Clip = mul(GetCameraProjection(), float4(ViewPos, 1.0));
    Clip.xyz /= Clip.w;
    return float3(Clip.xy * 0.5 + 0.5, Clip.z);
}

float3 SSR_ViewNormal(int2 Texel)
{
    const float3 WorldNormal = Pass().Surface.Load(int3(Texel, 0)).xyz;
    return normalize(mul((float3x3)GetCameraView(), WorldNormal));
}

// The surface's true orientation from its depth neighbors, which a normal map cannot tilt into the surface.
float3 SSR_GeometricNormal(int2 Texel, float DepthC, float3 ViewC, float2 Size)
{
    const int2 MaxTexel = int2(Size) - 1;
    const float4 H = float4(
        SSR_PyramidDepth(clamp(Texel + int2(-1, 0), int2(0, 0), MaxTexel), 0),
        SSR_PyramidDepth(clamp(Texel + int2(-2, 0), int2(0, 0), MaxTexel), 0),
        SSR_PyramidDepth(clamp(Texel + int2( 1, 0), int2(0, 0), MaxTexel), 0),
        SSR_PyramidDepth(clamp(Texel + int2( 2, 0), int2(0, 0), MaxTexel), 0));
    const float4 V = float4(
        SSR_PyramidDepth(clamp(Texel + int2(0, -1), int2(0, 0), MaxTexel), 0),
        SSR_PyramidDepth(clamp(Texel + int2(0, -2), int2(0, 0), MaxTexel), 0),
        SSR_PyramidDepth(clamp(Texel + int2(0,  1), int2(0, 0), MaxTexel), 0),
        SSR_PyramidDepth(clamp(Texel + int2(0,  2), int2(0, 0), MaxTexel), 0));

    // Each side extrapolates its far neighbor through the near one, and the side that predicts the center better is not across an edge.
    const float2 HErr = abs((2.0 * H.xz - H.yw) - DepthC);
    const float2 VErr = abs((2.0 * V.xz - V.yw) - DepthC);
    const int    HSign = HErr.x < HErr.y ? -1 : 1;
    const int    VSign = VErr.x < VErr.y ? -1 : 1;

    const float3 ViewH = SSR_ViewPos(float3((float2(Texel + int2(HSign, 0)) + 0.5) / Size, HSign < 0 ? H.x : H.z));
    const float3 ViewV = SSR_ViewPos(float3((float2(Texel + int2(0, VSign)) + 0.5) / Size, VSign < 0 ? V.x : V.z));

    float3 N = normalize(cross(float(VSign) * (ViewV - ViewC), float(HSign) * (ViewH - ViewC)));

    // The cross product's handedness depends on the projection, and a visible surface always faces the eye.
    if (dot(N, -ViewC) < 0.0)
    {
        N = -N;
    }
    return N;
}

// The full-resolution pixel the downsample kept for this texel, the closest of its block.
int2 SSR_KeptPixel(int2 Texel)
{
    const int2 MaxPixel = int2(Pass().ScreenW, Pass().ScreenH) - 1;
    const int  Scale    = int(Pass().Scale);
    const int2 Base     = Texel * Scale;

    // The color has to come from the surface the depth test hit, which the pyramid made one pixel wider than it is.
    float BestDepth = -1.0;
    int2  BestPixel = min(Base, MaxPixel);
    for (int i = 0; i < Scale * Scale; ++i)
    {
        const int2  Pixel = min(Base + int2(i % Scale, i / Scale), MaxPixel);
        const float Depth = Pass().Depth.Load(Pixel).r;
        if (Depth > BestDepth)
        {
            BestDepth = Depth;
            BestPixel = Pixel;
        }
    }
    return BestPixel;
}

struct FSSRResult
{
    float4 Color;
    float  MipLevel;
};

FSSRResult TraceTexel(int2 Texel)
{
    FSSRResult Result;
    Result.Color    = float4(0.0);
    Result.MipLevel = 0.0;

    const float2 Size = float2(Pass().TraceW, Pass().TraceH);

    float3 ScreenPos = float3((float2(Texel) + 0.5) / Size, SSR_PyramidDepth(Texel, 0));
    if (ScreenPos.z <= 0.0)
    {
        return Result;
    }

    const float Roughness = Pass().Surface.Load(int3(Texel, 0)).w;
    if (Roughness >= Pass().MaxRoughness)
    {
        return Result;
    }

    float3 Pos = SSR_ViewPos(ScreenPos);
    const float3 Normal     = SSR_ViewNormal(Texel);
    const float3 GeomNormal = SSR_GeometricNormal(Texel, ScreenPos.z, Pos, Size);

    // A normal-mapped pixel can point a ray back into its own surface, so the start is lifted off it in proportion.
    Pos       += GeomNormal * (1.0 - pow(saturate(dot(Normal, GeomNormal)), 8.0));
    ScreenPos  = SSR_ScreenPos(Pos);

    const float3 ViewDir = IsCameraOrthographic() ? float3(0.0, 0.0, 1.0) : normalize(Pos);
    float3 RayDir = normalize(reflect(ViewDir, Normal));
    if (dot(RayDir, GeomNormal) < 0.0)
    {
        RayDir = normalize(reflect(RayDir, GeomNormal));
    }

    // Projection inverts behind the near plane, so the direction sample stops there.
    float3 EndPos = Pos + RayDir;
    const float NearZ = GetNearPlane();
    if (EndPos.z < NearZ)
    {
        EndPos += RayDir * ((NearZ - EndPos.z) / min(RayDir.z, -1e-6));
    }

    // Unit screen-depth per t, so a pyramid depth converts to a ray parameter without a divide.
    float3 ScreenRay = SSR_ScreenPos(EndPos) - ScreenPos;
    ScreenRay /= max(abs(ScreenRay.z), 1e-8);

    // Reverse-Z, so depth rising along the ray means it is heading toward the camera.
    const bool bTowardCamera = ScreenRay.z >= 0.0;

    const float2 T0   = (float2(0.0) - ScreenPos.xy) / ScreenRay.xy;
    const float2 T1   = (float2(1.0) - ScreenPos.xy) / ScreenRay.xy;
    const float2 TFar = max(T0, T1);
    const float  TMax = min(TFar.x, TFar.y);

    const float2 CellStep = float2(ScreenRay.x < 0.0 ? -1.0 : 1.0, ScreenRay.y < 0.0 ? -1.0 : 1.0);
    const float2 CellBias = saturate(CellStep);

    // Starting at the next cell boundary keeps the ray from immediately intersecting the texel it left.
    float T;
    {
        const float2 Cell    = floor(ScreenPos.xy * Size);
        const float2 NextPos = (Cell + CellBias) / Size + CellStep * kCellEpsilon;
        const float2 PosT    = (NextPos - ScreenPos.xy) / ScreenRay.xy;
        T = min(PosT.x, PosT.y);
    }

    int  Level     = 0;
    uint Remaining = Pass().MaxIterations;
    const int TopLevel = int(Pass().MipCount) - 1;

    [loop]
    while (Level >= 0 && Remaining > 0u && T < TMax)
    {
        const float3 Cur       = ScreenPos + ScreenRay * T;
        const float2 CellCount = float2(max(uint2(Size) >> uint(Level), uint2(1u, 1u)));
        const float2 Cell      = floor(Cur.xy * CellCount);
        const float  CellDepth = SSR_PyramidDepth(int2(Cell), Level);
        const float  DepthT    = (CellDepth - ScreenPos.z) * ScreenRay.z;

        const float2 NextPos = (Cell + CellBias) / CellCount + CellStep * kCellEpsilon;
        const float2 PosT    = (NextPos - ScreenPos.xy) / ScreenRay.xy;
        const float  EdgeT   = min(PosT.x, PosT.y);

        bool bHit     = bTowardCamera ? (T <= DepthT) : (DepthT <= EdgeT);
        int  LevelDir = bHit ? -1 : 1;

        if (Level == 0 && (LinearizeSceneDepth(Cur.z) - LinearizeSceneDepth(CellDepth)) > Pass().DepthTolerance)
        {
            // Too far behind the surface to be it, and holding the level stops the search bouncing between two mips.
            bHit     = false;
            LevelDir = 0;
        }

        if (bHit)
        {
            if (!bTowardCamera)
            {
                T = max(T, DepthT);
            }
        }
        else
        {
            T = EdgeT;
        }

        Level = min(Level + LevelDir, TopLevel);
        --Remaining;
    }

    const float3 HitScreen = ScreenPos + ScreenRay * T;
    const int2   HitTexel  = clamp(int2(HitScreen.xy * Size), int2(0, 0), int2(Size) - 1);
    const float  HitDepth  = SSR_PyramidDepth(HitTexel, 0);

    // Only a walk that descended past the finest level found a surface; one that ran out of steps stopped wherever it was.
    const bool bConverged = Level < 0;

    // Graded rather than rejected, so a doubtful hit fades and still hands the blur its mip level.
    float Validity = (!bConverged || T >= TMax || HitDepth <= 0.0) ? 0.0 : 1.0;

    // A ray that barely moved can only be trusted if it did not land on the back of what it left.
    if (all(abs(ScreenRay.xy * T) < 2.0 / Size) && dot(RayDir, SSR_ViewNormal(HitTexel)) >= 0.0)
    {
        Validity = 0.0;
    }

    const float Gap        = length(SSR_ViewPos(HitScreen) - SSR_ViewPos(float3(HitScreen.xy, HitDepth)));
    const float Confidence = 1.0 - smoothstep(0.0, Pass().DepthTolerance, Gap);
    Validity *= saturate(Confidence * Confidence);

    // Fades toward the screen border along both axes at once, so corners do not end in a hard seam.
    const float2 HitPixel   = HitScreen.xy * Size;
    const float  Margin     = (Size.x + Size.y) * 0.05;
    const float2 FromBorder = select(HitPixel < Size * 0.5, HitPixel, Size - HitPixel);
    Validity *= smoothstep(0.0, Margin * Margin, FromBorder.x * FromBorder.y);

    // A walk that gave up partway is a miss, so it asks for the same blur as a ray that reached the screen edge.
    const float RayLength = length(ScreenRay.xy * (bConverged ? T : TMax));
    const float FadeIn    = Pass().FadeIn  == 0.0 ? 1.0 : pow(saturate(RayLength),       Pass().FadeIn);
    const float FadeOut   = Pass().FadeOut == 0.0 ? 1.0 : pow(saturate(1.0 - RayLength), Pass().FadeOut);
    // Anything short of exactly 1 lets a strong specular highlight leak through the reflection.
    const float Fade      = (FadeIn * FadeOut > 0.999) ? 1.0 : FadeIn * FadeOut;
    Validity *= Fade;

    if (Validity > 0.0)
    {
        float3 Radiance = SanitizeRadiance(Pass().SceneColor.Load(SSR_KeptPixel(HitTexel)).rgb);

        // Compressed so the roughness blur averages bright and dark hits evenly, and the resolve undoes it.
        Radiance /= 1.0 + dot(Radiance, float3(0.2126, 0.7152, 0.0722));
        Result.Color = float4(Radiance, 1.0) * Validity;
    }

    if (Roughness > 0.001)
    {
        // The widest sphere that fits inside the reflection cone, which keeps the blur from reaching past what the lobe covers.
        const float ConeAngle  = min(Roughness, 0.999) * kPi * 0.5;
        const float ConeLength = max(RayLength, 1e-6);
        const float Opposite   = 2.0 * tan(ConeAngle) * ConeLength;
        const float BlurRadius = (Opposite * (sqrt(Opposite * Opposite + 4.0 * ConeLength * ConeLength) - Opposite)) / (4.0 * ConeLength);

        // A mip is twice as wide as the one below it, so the level is the log of the blur diameter in texels.
        Result.MipLevel = clamp(log2(BlurRadius * max(Size.x, Size.y) / 16.0 + 1.0), 0.0, float(TopLevel));
    }

    // Invalid texels still write a level so rough transitions stay smooth, which piles up near a ray's far end unless faded.
    Result.MipLevel *= pow(saturate(1.25 - RayLength), 0.2);

    // Without temporal accumulation the half-resolution hit edge would alias, so every reflection gets at least one level of blur.
    Result.MipLevel = clamp(Result.MipLevel, kMinMipLevel, float(TopLevel));
    return Result;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= Pass().TraceW || DTid.y >= Pass().TraceH)
    {
        return;
    }

    const FSSRResult Result = TraceTexel(int2(DTid.xy));
    Pass().OutColor[int2(DTid.xy)] = Result.Color;
    Pass().OutMip[int2(DTid.xy)]   = float4(Result.MipLevel, 0.0, 0.0, 0.0);
}
