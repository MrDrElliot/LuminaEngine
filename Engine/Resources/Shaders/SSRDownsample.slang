// A compute lane has no rasterizer, so nothing here may use ddx/ddy or an implicit-derivative sample.
#define SHADING_COMPUTE 1

#include "Includes/SurfaceShading.slang"
#include "Includes/VisBufferSurface.slang"
#include "Includes/GlobalRHIStorage.slang"

// Keeps the closest pixel of each Scale x Scale block and that same pixel's normal and roughness, so no trace texel blends two surfaces.

struct FSSRDownsampleArgs
{
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint GBufferCIndex;
    uint GBufferDIndex;

    uint DepthIndex;
    uint SlotImageIndex;  // slot + 1 per pixel from the classify count, 0 where nothing deferred shades it
    uint OutDepthUAV;     // mip 0 of the closest-depth pyramid
    uint OutSurfaceUAV;   // world normal in xyz, roughness in w, roughness 1 where nothing may reflect

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
        const float Depth = LoadTexture2D(Pass().DepthIndex, Pixel).r;
        if (Depth > BestDepth)
        {
            BestDepth = Depth;
            BestPixel = Pixel;
        }
    }

    float4 Surface = float4(0.0, 0.0, 1.0, 1.0);
    if (BestDepth > 0.0 && LoadTexture2D(Pass().SlotImageIndex, BestPixel).r >= 0.5)
    {
        FGBufferTargets Targets;
        Targets.A = Pass().GBufferAIndex;
        Targets.B = Pass().GBufferBIndex;
        Targets.C = Pass().GBufferCIndex;
        Targets.D = Pass().GBufferDIndex;

        const FGBufferData G = LoadGBuffer(Targets, BestPixel);
        if (!IsUnlit(G))
        {
            Surface = float4(G.WorldNormal, max(LoadTexture2D(Targets.C, BestPixel).r, 0.045));
        }
    }

    gRWTextures2D[Pass().OutDepthUAV][int2(DTid.xy)]   = float4(BestDepth, 0.0, 0.0, 0.0);
    gRWTextures2D[Pass().OutSurfaceUAV][int2(DTid.xy)] = Surface;
}
