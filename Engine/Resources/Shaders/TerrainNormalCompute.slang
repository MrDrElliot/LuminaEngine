#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"

// Matches FTerrainNormalParams in TerrainRenderTypes.h.
struct FTerrainNormalParams
{
    int     Resolution;
    int     RegionMinX;     // Inclusive.
    int     RegionMinY;
    int     RegionSizeX;

    int     RegionSizeY;
    float   TileWorldSize;
    float   MaxHeight;
    int     _Pad;
};

// Matches FTerrainNormalArgs in ForwardRenderScene.cpp (TerrainUpdatePass): scalar
// params plus heightmap SRV / normal UAV heap indices.
struct FTerrainNormalArgs
{
    FTerrainNormalParams Params;
    uint HeightmapIndex;
    uint NormalUAV;
    uint _Pad0;
    uint _Pad1;
};

FTerrainNormalArgs* Pass() { return GetPassArgs<FTerrainNormalArgs>(); }

// Central-difference normals. Heightmap sampled clamp-to-edge so border pixels stay stable.
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    FTerrainNormalParams Params = Pass().Params;

    if ((int)DTid.x >= Params.RegionSizeX || (int)DTid.y >= Params.RegionSizeY)
        return;

    int X = Params.RegionMinX + (int)DTid.x;
    int Y = Params.RegionMinY + (int)DTid.y;

    float InvRes = 1.0 / float(Params.Resolution);
    float Stride = Params.TileWorldSize / float(Params.Resolution - 1);

    float HL = SampleTexture2DLevel(Pass().HeightmapIndex, SAMPLER_LINEAR_CLAMP, float2(float(X - 1) + 0.5, float(Y    ) + 0.5) * InvRes, 0).r;
    float HR = SampleTexture2DLevel(Pass().HeightmapIndex, SAMPLER_LINEAR_CLAMP, float2(float(X + 1) + 0.5, float(Y    ) + 0.5) * InvRes, 0).r;
    float HD = SampleTexture2DLevel(Pass().HeightmapIndex, SAMPLER_LINEAR_CLAMP, float2(float(X    ) + 0.5, float(Y - 1) + 0.5) * InvRes, 0).r;
    float HU = SampleTexture2DLevel(Pass().HeightmapIndex, SAMPLER_LINEAR_CLAMP, float2(float(X    ) + 0.5, float(Y + 1) + 0.5) * InvRes, 0).r;

    float dHdx = (HR - HL) * Params.MaxHeight / (2.0 * Stride);
    float dHdz = (HU - HD) * Params.MaxHeight / (2.0 * Stride);

    // Surface tangents T=(1,dHdx,0), B=(0,dHdz,1). Cross = (-dHdx, 1, -dHdz) * norm.
    float3 Normal = normalize(float3(-dHdx, 1.0, -dHdz));

    // Pack XZ slope into [0,1]; Y is reconstructed in the vertex shader.
    float2 Packed = Normal.xz * 0.5 + 0.5;
    gRWTextures2D[Pass().NormalUAV][int2(X, Y)] = float4(Packed.x, Packed.y, 0.0, 1.0);
}
