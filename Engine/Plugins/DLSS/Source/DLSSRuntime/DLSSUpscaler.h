#pragma once

#include "Core/Threading/Thread.h"
#include "World/Scene/RenderScene/SceneUpscaler.h"

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace Lumina
{
    // DLSS Super Resolution through NGX on Vulkan, with each engine upscaler mode on its DLSS counterpart.
    class FDLSSUpscaler final : public IUpscaler
    {
    public:

        ~FDLSSUpscaler() override;

        FName GetName() const override { return FName("DLSS"); }
        bool  IsSupported() const override;
        bool  IsTemporal() const override { return true; }
        float GetRenderScale(const FUIntVector2& DisplaySize, EUpscalerMode Mode, float RequestedScale) const override;
        void  Evaluate(RHI::FCmdListH CL, const FUpscaleInputs& Inputs) override;
        void  ReleaseViewResources() override;

        // Waits for the GPU and releases the feature and NGX, which must happen while the device still exists.
        void Shutdown();

    private:

        // NGX can only start once the renderer has a device, so the first caller after that starts it.
        bool EnsureInitialized() const;

        void ReleaseFeature();

        mutable FMutex               Mutex;
        mutable bool                 bInitAttempted = false;
        mutable bool                 bAvailable     = false;
        mutable void*                InitializedDevice = nullptr;
        mutable NVSDK_NGX_Parameter* Parameters     = nullptr;

        NVSDK_NGX_Handle* Feature        = nullptr;
        FUIntVector2      FeatureRender  = FUIntVector2(0);
        FUIntVector2      FeatureDisplay = FUIntVector2(0);
        int32             FeatureQuality = -1;
    };
}
