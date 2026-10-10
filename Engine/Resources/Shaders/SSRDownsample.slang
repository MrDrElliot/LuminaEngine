// A compute lane has no rasterizer, so nothing here may use ddx/ddy or an implicit-derivative sample.
#define SHADING_COMPUTE 1

#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHI.slang"

// Keeps the closest pixel of each Scale x Scale block and that same pixel's normal and roughness, so no trace texel blends two surfaces.

struct FSSRDownsampleArgs
{
    FGBufferTargets GBuffer;

    FTexture2D   Depth;
    FTexture2D   SlotImage;  // slot + 1 per pixel from the classify count, 0 where nothing deferred shades it
    FRWTexture2D OutDepth;     // mip 0 of the closest-depth pyramid
    FRWTexture2D OutSurface;   // world normal in xyz, roughness in w, roughness 1 where nothing may reflect

    uint ScreenW;
    uint ScreenH;
    uint TraceW;
    uint TraceH;

    uint Scale;           // screen pixels per trace texel along each axis
    uint _Pad0;
    uint _Pad1;
    uint _Pad2;
};
FSSRDownsampleArgs* Pass() { return GetPassArgs<FSSRDownsampleArgs>(); }

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= Pass().TraceW || DTid.y >= Pass().TraceH)
    {
        return;
    }

    const int2 MaxPixel = int2(Pass().ScreenW, Pass().ScreenH) - 1;
    const int  Scale    = int(Pass().Scale);
    const int2 Base     = int2(DTid.xy) * Scale;

    // Reverse-Z, so the largest raw depth is the closest surface.
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

    float4 Surface = float4(0.0, 0.0, 1.0, 1.0);
    if (BestDepth > 0.0 && Pass().SlotImage.Load(BestPixel).r >= 0.5)
    {
        const FGBufferTargets Targets = Pass().GBuffer;

        const FGBufferData G = LoadGBuffer(Targets, BestPixel);
        if (!IsUnlit(G))
        {
            Surface = float4(G.WorldNormal, max(Targets.C.Load(BestPixel).r, 0.045));
        }
    }

    Pass().OutDepth[int2(DTid.xy)]   = float4(BestDepth, 0.0, 0.0, 0.0);
    Pass().OutSurface[int2(DTid.xy)] = Surface;
}
