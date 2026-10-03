#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"

// One level of the closest-depth pyramid the reflection trace skips empty space with.

struct FSSRChainLevelArgs
{
    uint ChainIndex;  // the whole chain, read at SourceMip
    uint SourceMip;
    uint DestUAV;
    uint DestMip;

    uint SourceW;
    uint SourceH;
    uint DestW;
    uint DestH;
};
FSSRChainLevelArgs* Pass() { return GetPassArgs<FSSRChainLevelArgs>(); }

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= Pass().DestW || DTid.y >= Pass().DestH)
    {
        return;
    }

    const int2 Base = int2(DTid.xy) * 2;
    const int2 Last = int2(Pass().SourceW, Pass().SourceH) - 1;

    // A level rounds its size down, so an odd parent's last row and column fold into the edge texel.
    const int2 Extra = int2((DTid.x == Pass().DestW - 1u && (Pass().SourceW & 1u) != 0u) ? 1 : 0,
                            (DTid.y == Pass().DestH - 1u && (Pass().SourceH & 1u) != 0u) ? 1 : 0);
    const int2 End   = min(Base + 1 + Extra, Last);

    // Reverse-Z, so the closest surface in the cell is the largest depth.
    float Closest = 0.0;
    for (int y = Base.y; y <= End.y; ++y)
    {
        for (int x = Base.x; x <= End.x; ++x)
        {
            Closest = max(Closest, gTextures2D[Pass().ChainIndex].Load(int3(x, y, int(Pass().SourceMip))).r);
        }
    }

    gRWTextures2D[Pass().DestUAV][int2(DTid.xy)] = float4(Closest, 0.0, 0.0, 0.0);
}
