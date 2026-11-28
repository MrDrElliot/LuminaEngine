#pragma once
#include "Includes/SceneGlobals.slang"

// Must match FSceneLightSet::FUpload.
struct FLightUpload
{
    FLight Light;
    uint   Slot;
    uint   Layers;
    uint   _Pad0;
    uint   _Pad1;
};

struct FLightUploadArgs
{
    GPUSpan<FLightUpload> Uploads;
};

FLightUploadArgs* Pass() { return GetPassArgs<FLightUploadArgs>(); }

// Writes the slots that changed since the last frame into the retained set every view culls.
[shader("compute")]
[numthreads(LIGHT_CULL_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 DispatchID : SV_DispatchThreadID)
{
    const uint Index = DispatchID.x;
    if (!Pass().Uploads.Contains(Index))
    {
        return;
    }

    const FLightUpload Upload = Pass().Uploads.GetAligned(Index);
    if (RetainedLights().Contains(Upload.Slot))
    {
        RetainedLights().At(Upload.Slot)[0] = Upload.Light;
    }
    if (RetainedLightBounds().Contains(Upload.Slot))
    {
        RetainedLightBounds().At(Upload.Slot)[0] = LightBoundingSphere(Upload.Light);
    }
    if (RetainedLightLayers().Contains(Upload.Slot))
    {
        RetainedLightLayers().At(Upload.Slot)[0] = Upload.Layers;
    }
}
