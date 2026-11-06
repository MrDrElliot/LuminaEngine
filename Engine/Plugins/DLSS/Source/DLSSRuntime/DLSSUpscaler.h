#pragma once

#include "Core/Threading/Thread.h"
#include "Config/EngineSettings.h"
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

        EUpscaler GetType() const override { return EUpscaler::DLSS; }
        bool  IsSupported() const override;
        bool  IsTemporal() const override { return true; }
        float GetRenderScale(const FUIntVector2& DisplaySize, EUpscalerMode Mode, float RequestedScale) const override;
        void  Evaluate(RHI::FCmdListH CL, const FUpscaleInputs& Inputs) override;
        void  ReleaseViewResources(const void* ViewKey) override;

        // Waits for the GPU and releases every feature and NGX, which must happen while the device still exists.
        void Shutdown();

    private:

        struct FFeature
        {
            NVSDK_NGX_Handle* Handle  = nullptr;
            FUIntVector2      Render  = FUIntVector2(0);
            FUIntVector2      Display = FUIntVector2(0);
            int32             Quality = -1;
        };

        // NGX can only start once the renderer has a device, so the first caller after that starts it.
        bool EnsureInitialized() const;

        static void ReleaseFeature(FFeature& Feature);

        mutable FMutex               Mutex;
        mutable bool                 bInitAttempted = false;
        mutable bool                 bAvailable     = false;
        mutable void*                InitializedDevice = nullptr;
        mutable NVSDK_NGX_Parameter* Parameters     = nullptr;

        THashMap<const void*, FFeature> Features;
    };
}
