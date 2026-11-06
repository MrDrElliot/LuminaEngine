#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Weather.slang"
#include "Includes/RainCommon.slang"
#include "Includes/VisBufferSurface.slang"

struct FPushConstants
{
    GPUSpan<FRainSplash> Splashes;
    uint                 Cursor;
    uint                 Count;
    uint                 FrameSeed;
    float                SpawnWindow;
    FTexture2D           GBufferB;
    FTexture2DUInt       VisBuffer;
    FTexture2DUInt       SlotImage;
    uint                 _Pad0;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

float3 PixelWorldPosition(int2 Pixel, uint2 Size, out bool bSky)
{
    const float Raw = Scene().SceneDepth.Load(int3(Pixel, 0)).r;
    bSky = Raw <= 0.0;
    const float2 UV = (float2(Pixel) + 0.5) / float2(Size);
    return ReconstructWorldPos(UV, Raw);
}

// Takes each axis from whichever neighbor lies on the same surface, so silhouettes do not tip the normal on its side.
float3 DepthNormal(int2 Pixel, uint2 Size, float3 Center)
{
    bool bSkyL, bSkyR, bSkyU, bSkyD;
    const float3 Left  = PixelWorldPosition(Pixel - int2(1, 0), Size, bSkyL);
    const float3 Right = PixelWorldPosition(Pixel + int2(1, 0), Size, bSkyR);
    const float3 Up    = PixelWorldPosition(Pixel - int2(0, 1), Size, bSkyU);
    const float3 Down  = PixelWorldPosition(Pixel + int2(0, 1), Size, bSkyD);

    const float3 ToLeft  = Center - Left;
    const float3 ToRight = Right - Center;
    const float3 ToUp    = Center - Up;
    const float3 ToDown  = Down - Center;
    const float3 DX = (bSkyR || (!bSkyL && dot(ToLeft, ToLeft) < dot(ToRight, ToRight))) ? ToLeft : ToRight;
    const float3 DY = (bSkyD || (!bSkyU && dot(ToUp, ToUp) < dot(ToDown, ToDown))) ? ToUp : ToDown;
    return normalize(cross(DY, DX));
}

// Spawns splashes at random visible pixels, which lands them on every surface the camera sees including terrain.
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= Pass().Count || !HasWeather() || !Scene().SceneDepth.IsValid() || Pass().Splashes.IsEmpty())
    {
        return;
    }

    const FWeatherData W = GetWeather();
    uint Seed = PCGHash(Pass().FrameSeed ^ (DTid.x * 0x27D4EB2Du));

    // Kept off the border, where a neighbor would clamp onto the pixel itself and flatten the normal sideways.
    const uint2 Size  = GetScreenSize();
    const int2  Pixel = int2(1, 1) + int2(float2(WeatherRandom(Seed), WeatherRandom(Seed)) * float2(Size - 2u));

    bool bSky;
    const float3 Center = PixelWorldPosition(Pixel, Size, bSky);
    if (bSky || length(Center - GetCameraPosition()) > W.RainTint.w)
    {
        return;
    }

    if (Pass().VisBuffer.IsValid())
    {
        const uint Instance = ResolveVisInstance(Pass().VisBuffer.Load(Pixel), Scene().CullData.MeshletDrawListCapacity);
        if (Instance != ~0u && FGPUInstance::HasFlag(Instances() + Instance, EInstanceFlags::Foliage))
        {
            return;
        }
    }

    float3 Normal;
    if (Pass().SlotImage.IsValid() && Pass().SlotImage.Load(Pixel) != 0u)
    {
        Normal = OctDecode01(Pass().GBufferB.Load(Pixel).rg);
    }
    else
    {
        Normal = DepthNormal(Pixel, Size, Center);
    }
    Normal = Normal.y < 0.0 ? -Normal : Normal;
    if (Normal.y < 0.35)
    {
        return;
    }

    if (WeatherRandom(Seed) >= RainExposure(W, Center))
    {
        return;
    }

    FRainSplash Splash;
    Splash.Position      = Center;
    Splash.SpawnTime     = W.Surface2.z - WeatherRandom(Seed) * Pass().SpawnWindow;
    Splash.PackedNormal  = PackNormal(Normal);
    const float3 Light   = RainLightAt(Center + Normal * 0.05);
    Splash.PackedLightRG = PackHalf2(Light.rg);
    Splash.PackedLightB  = f32tof16(Light.b);
    Splash.Seed          = PCGHash(Seed) | 1u;

    const uint Capacity = Pass().Splashes.Count;
    Pass().Splashes.Set((Pass().Cursor + DTid.x) % Capacity, Splash);
}
