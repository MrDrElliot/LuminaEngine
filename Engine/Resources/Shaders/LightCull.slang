#pragma once
#include "Includes/SceneGlobals.slang"

#define LOCAL_SIZE 128

bool SphereAABBIntersection(float3 Center, float Radius, float3 AABBMin, float3 AABBMax)
{
    float3 ClosestPoint    = clamp(Center, AABBMin, AABBMax);
    float3 Delta           = ClosestPoint - Center;
    float  DistanceSquared = dot(Delta, Delta);
    return DistanceSquared <= Radius * Radius;
}

// View-space light position (xyz) + radius (w), staged once per batch so the group's clusters
// share one transform and one global load per light instead of each cluster re-reading every light.
groupshared float4 gLightViewPosRadius[LOCAL_SIZE];

[shader("compute")]
[numthreads(LOCAL_SIZE, 1, 1)]
void ComputeMain(uint3 WorkGroupID : SV_GroupID, uint3 LocalID : SV_GroupThreadID)
{
    uint Index = WorkGroupID.x * LOCAL_SIZE + LocalID.x;
    uint Local = LocalID.x;

    // Tail-thread guard: out-of-range threads still participate in the
    // cooperative loads and barriers below, they just never test or write.
    uint3 Grid          = Scene().GridSize.xyz;
    uint  NumClusters   = Grid.x * Grid.y * Grid.z;
    bool  bValidCluster = Index < NumClusters;

    float3 ClusterMin = bValidCluster ? loadAligned<16>(&Clusters()[Index].MinPoint).xyz : float3(0.0);
    float3 ClusterMax = bValidCluster ? loadAligned<16>(&Clusters()[Index].MaxPoint).xyz : float3(0.0);

    uint LightCount = LightData().NumLights;

    // Directional light is always at index 0; skip since it applies globally.
    uint StartIndex = (LightCount > 0 && HasFlag(LightData().Lights[0].Flags, ELightFlags::Directional)) ? 1u : 0u;

    float4x4 View         = GetCameraView();
    uint     VisibleCount = 0;

    // LightCount is group-uniform, so every thread runs the same iterations and reaches both
    // barriers each pass -- the syncs are never in divergent control flow.
    for (uint Base = 0; Base < LightCount; Base += LOCAL_SIZE)
    {
        uint LoadIndex = Base + Local;
        if (LoadIndex < LightCount)
        {
            float3 ViewPos = mul(View, float4(LightData().Lights[LoadIndex].Position, 1.0)).xyz;
            gLightViewPosRadius[Local] = float4(ViewPos, LightData().Lights[LoadIndex].Radius);
        }
        else
        {
            gLightViewPosRadius[Local] = float4(0.0, 0.0, 0.0, -1.0);
        }

        GroupMemoryBarrierWithGroupSync();

        if (bValidCluster && VisibleCount < LIGHTS_PER_CLUSTER)
        {
            uint BatchCount = min((uint)LOCAL_SIZE, LightCount - Base);
            for (uint j = 0; j < BatchCount; ++j)
            {
                uint GlobalLight = Base + j;
                if (GlobalLight < StartIndex)
                {
                    continue;
                }

                float4 LightVP = gLightViewPosRadius[j];
                if (SphereAABBIntersection(LightVP.xyz, LightVP.w, ClusterMin, ClusterMax))
                {
                    Clusters()[Index].LightIndices[VisibleCount] = GlobalLight;
                    ++VisibleCount;
                    if (VisibleCount >= LIGHTS_PER_CLUSTER)
                    {
                        break;
                    }
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if (bValidCluster)
    {
        Clusters()[Index].Count = VisibleCount;
    }
}
