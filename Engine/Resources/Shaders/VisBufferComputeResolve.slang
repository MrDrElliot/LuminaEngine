#include "Includes/SceneGlobals.slang"

// Merges the compute raster's nearest depth and VisID into the VisBuffer under the same depth test the hardware draws used, and empties what it read.

struct FVisBufferComputeResolveArgs
{
    GPUSpan<uint64_t> Target;
    uint2             TargetSize;
    uint              _Pad0;
    uint              _Pad1;
};
FVisBufferComputeResolveArgs* Pass() { return GetPassArgs<FVisBufferComputeResolveArgs>(); }

struct FResolveOutput
{
    uint  VisID : SV_Target0;
    float Depth : SV_Depth;
};

[shader("fragment")]
FResolveOutput FragmentMain(float4 Position : SV_Position)
{
    const uint2 Pixel = uint2(Position.xy);
    const uint  Index = Pixel.y * Pass().TargetSize.x + Pixel.x;
    const uint64_t Packed = Pass().Target.Get(Index);
    if (Packed == 0ull)
    {
        discard;
    }

    // Emptied here, so the next phase or frame starts from nothing without a separate clear.
    Pass().Target.Set(Index, 0ull);

    FResolveOutput Out;
    Out.VisID = uint(Packed & 0xFFFFFFFFull);
    Out.Depth = asfloat(uint(Packed >> 32));
    return Out;
}
