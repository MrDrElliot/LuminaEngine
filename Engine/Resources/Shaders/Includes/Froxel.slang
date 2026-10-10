#pragma once

// Shared froxel-volume math for the volumetric fog passes. Pure functions only, no SceneGlobals.

// Mirrors the cluster Z slicing so froxels align with the clustered light grid.
float Froxel_SliceDistance(float SliceF, float GridZ, float NearP, float Range)
{
    return NearP * pow(Range / NearP, SliceF / GridZ);
}

// Inverse of Froxel_SliceDistance, giving a normalized froxel W in [0,1].
float Froxel_DistanceToW(float ViewDist, float NearP, float Range)
{
    return saturate(log(max(ViewDist, NearP) / NearP) / log(Range / NearP));
}
