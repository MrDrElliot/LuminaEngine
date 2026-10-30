#pragma once
#include "SceneGlobals.slang"
#include "Sky.slang"

// One half-res cloud texel's view ray clipped to the cloud shell and the scene, shared by the march and its temporal resolve.
struct FCloudRay
{
    float3 Origin;      // planet-relative camera position
    float3 Dir;
    float  Start;
    float  End;
    float  RawDepth;    // closest scene depth under the texel's footprint, 0 where it sees sky
    float  InnerR;
    float  OuterR;
};

FCloudRay Cloud_SetupRay(FTexture2D Depth, int2 P, uint2 Size, float LayerBottom, float LayerTop, float MaxDistance)
{
    FCloudRay Ray;
    float2 UV = (float2(P) + 0.5) / float2(Size);

    // One max-reduction tap at the half-res texel center covers its 2x2 full-res footprint; reverse-Z, so max is the closest surface.
    Ray.RawDepth = Depth.SampleLevel(Samplers::MaxReduction, UV, 0.0).r;

    float2 NDC     = UV * 2.0 - 1.0;
    float4 ViewH   = mul(GetInverseCameraProjection(), float4(NDC, 1.0, 1.0));
    float3 ViewRay = ViewH.xyz / ViewH.w;
    Ray.Dir        = normalize(mul(GetInverseCameraView(), float4(ViewRay, 0.0)).xyz);

    // Planet-relative, so the layer curves away to the horizon instead of running to infinity.
    float3 CamPos = GetCameraPosition();
    Ray.Origin = float3(0.0, kPlanetR + CamPos.y, 0.0);
    Ray.InnerR = kPlanetR + LayerBottom;
    Ray.OuterR = kPlanetR + max(LayerTop, LayerBottom + 1.0);
    Ray.Start  = 0.0;
    Ray.End    = 0.0;

    float2 Outer = Sky_RaySphereIntersect(Ray.Origin, Ray.Dir, Ray.OuterR);
    if (Outer.y <= 0.0)
    {
        return Ray;
    }

    float2 Inner = Sky_RaySphereIntersect(Ray.Origin, Ray.Dir, Ray.InnerR);
    if (Inner.x > 0.0)
    {
        Ray.Start = max(Outer.x, 0.0);
        Ray.End   = Inner.x;
    }
    else if (Inner.y > 0.0)
    {
        Ray.Start = Inner.y;
        Ray.End   = Outer.y;
    }
    else
    {
        Ray.Start = max(Outer.x, 0.0);
        Ray.End   = Outer.y;
    }

    if (Ray.RawDepth > 0.0)
    {
        float3 SceneView  = ReconstructViewPos(UV, Ray.RawDepth, GetInverseCameraProjection());
        float3 SceneWorld = mul(GetInverseCameraView(), float4(SceneView, 1.0)).xyz;
        Ray.End = min(Ray.End, length(SceneWorld - CamPos));
    }

    Ray.End = min(Ray.End, MaxDistance);
    return Ray;
}
