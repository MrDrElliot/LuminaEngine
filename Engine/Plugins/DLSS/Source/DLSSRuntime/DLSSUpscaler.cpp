#include "DLSSUpscaler.h"

#include "Config/EngineSettings.h"
#include "Core/Console/ConsoleVariable.h"
#include "Log/Log.h"
#include "Lumina.h"
#include "Paths/Paths.h"
#include "Renderer/RHI.h"
#include "Renderer/RHINative.h"

// Vulkan lives only in this plugin, and RHINative hands out opaque handles reinterpreted here.
#include <volk/volk.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>

#include <filesystem>
#include <system_error>

#include <Windows.h>

namespace Lumina
{
    namespace
    {
        TConsoleVar<int32> CVarDLSSLog("r.DLSS.Log", 0,
            "NGX's own logging routed to the engine log when NGX starts, 0 off, 1 on, 2 verbose.");

        // Identifies this engine to NGX in place of an NVIDIA application id.
        constexpr const char* kNGXProjectId = "3f2a8c71-6d4e-4b9a-a1c5-7e0d92b4f6a8";

        // NGX looks for nvngx_dlss.dll on this list, and the build stages it beside the executable.
        std::wstring GetExecutableDirectory()
        {
            wchar_t Buffer[MAX_PATH] = {};
            const DWORD Length = GetModuleFileNameW(nullptr, Buffer, MAX_PATH);
            return std::filesystem::path(std::wstring(Buffer, Length)).parent_path().wstring();
        }

        void NVSDK_CONV OnNGXLog(const char* Message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
        {
            LOG_INFO("[DLSS] NGX {}", Message);
        }

        // The DLSS modes by the fraction of the display they render, so a screen percentage lands on the nearest one.
        NVSDK_NGX_PerfQuality_Value QualityForScale(float Scale)
        {
            if (Scale >= 0.99f)
            {
                return NVSDK_NGX_PerfQuality_Value_DLAA;
            }
            if (Scale >= 0.62f)
            {
                return NVSDK_NGX_PerfQuality_Value_MaxQuality;
            }
            if (Scale >= 0.54f)
            {
                return NVSDK_NGX_PerfQuality_Value_Balanced;
            }
            if (Scale >= 0.42f)
            {
                return NVSDK_NGX_PerfQuality_Value_MaxPerf;
            }
            return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        }

        NVSDK_NGX_PerfQuality_Value QualityForMode(EUpscalerMode Mode, float Scale)
        {
            switch (Mode)
            {
            case EUpscalerMode::NativeAA:         return NVSDK_NGX_PerfQuality_Value_DLAA;
            case EUpscalerMode::Quality:          return NVSDK_NGX_PerfQuality_Value_MaxQuality;
            case EUpscalerMode::Balanced:         return NVSDK_NGX_PerfQuality_Value_Balanced;
            case EUpscalerMode::Performance:      return NVSDK_NGX_PerfQuality_Value_MaxPerf;
            case EUpscalerMode::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
            case EUpscalerMode::Custom:           break;
            }
            return QualityForScale(Scale);
        }

        NVSDK_NGX_Resource_VK MakeResource(const FRenderTexture& Texture, bool bDepth, bool bWritable)
        {
            const RHI::Native::FNativeTexture Native = RHI::Native::GetNativeTexture(Texture.Texture);

            VkImageSubresourceRange Range = {};
            Range.aspectMask     = bDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            Range.baseMipLevel   = 0;
            Range.levelCount     = 1;
            Range.baseArrayLayer = 0;
            Range.layerCount     = 1;

            return NVSDK_NGX_Create_ImageView_Resource_VK(
                reinterpret_cast<VkImageView>(Native.View), reinterpret_cast<VkImage>(Native.Image), Range,
                static_cast<VkFormat>(Native.Format), Texture.Extent.x, Texture.Extent.y, bWritable);
        }
    }

    FDLSSUpscaler::~FDLSSUpscaler()
    {
        Shutdown();
    }

    bool FDLSSUpscaler::EnsureInitialized() const
    {
        if (bInitAttempted)
        {
            return bAvailable;
        }

        const RHI::Native::FNativeDeviceHandles Handles = RHI::Native::GetNativeDeviceHandles();
        if (Handles.Device == nullptr || Handles.Backend != RHI::EBackend::Vulkan)
        {
            return false;
        }
        bInitAttempted = true;

        std::error_code Error;
        const std::filesystem::path DataPath = std::filesystem::path(Paths::GetUserDataDirectory().c_str()) / "Lumina" / "NGX";
        std::filesystem::create_directories(DataPath, Error);

        const std::wstring ExecutableDirectory = GetExecutableDirectory();
        const wchar_t* SearchPaths[] = { ExecutableDirectory.c_str() };

        NVSDK_NGX_FeatureCommonInfo CommonInfo = {};
        CommonInfo.PathListInfo.Path   = SearchPaths;
        CommonInfo.PathListInfo.Length = 1;
        CommonInfo.LoggingInfo.LoggingCallback          = OnNGXLog;
        CommonInfo.LoggingInfo.MinimumLoggingLevel      = (NVSDK_NGX_Logging_Level)Math::Clamp(CVarDLSSLog.GetValue(), 0, 2);
        CommonInfo.LoggingInfo.DisableOtherLoggingSinks = true;

        NVSDK_NGX_Result Result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
            kNGXProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, LUMINA_VERSION, DataPath.c_str(),
            static_cast<VkInstance>(Handles.Instance), static_cast<VkPhysicalDevice>(Handles.PhysicalDevice),
            static_cast<VkDevice>(Handles.Device),
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(Handles.GetInstanceProcAddr),
            reinterpret_cast<PFN_vkGetDeviceProcAddr>(Handles.GetDeviceProcAddr), &CommonInfo);
        if (NVSDK_NGX_FAILED(Result))
        {
            LOG_WARN("[DLSS] NGX did not start ({:#x}), so the built-in upscale stays in use. DLSS needs an NVIDIA RTX GPU.", (uint32)Result);
            return false;
        }
        InitializedDevice = Handles.Device;

        Result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&Parameters);
        if (NVSDK_NGX_FAILED(Result) || Parameters == nullptr)
        {
            LOG_WARN("[DLSS] NGX capability query failed ({:#x}).", (uint32)Result);
            return false;
        }

        int32 bSuperSampling = 0;
        Result = NVSDK_NGX_Parameter_GetI(Parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &bSuperSampling);
        if (NVSDK_NGX_FAILED(Result) || bSuperSampling == 0)
        {
            int32 InitResult = 0;
            NVSDK_NGX_Parameter_GetI(Parameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &InitResult);
            LOG_WARN("[DLSS] DLSS Super Resolution is unavailable on this GPU or driver ({:#x}).", (uint32)InitResult);
            return false;
        }

        bAvailable = true;
        LOG_INFO("[DLSS] NGX started and DLSS Super Resolution is available.");
        return true;
    }

    bool FDLSSUpscaler::IsSupported() const
    {
        FScopeLock Lock(Mutex);
        return EnsureInitialized();
    }

    float FDLSSUpscaler::GetRenderScale(const FUIntVector2& DisplaySize, EUpscalerMode Mode, float RequestedScale) const
    {
        FScopeLock Lock(Mutex);
        if (!EnsureInitialized() || DisplaySize.x == 0 || DisplaySize.y == 0)
        {
            return RequestedScale;
        }

        uint32 OptimalWidth  = 0;
        uint32 OptimalHeight = 0;
        uint32 MaxWidth = 0, MaxHeight = 0, MinWidth = 0, MinHeight = 0;
        float  Sharpness = 0.0f;
        const NVSDK_NGX_Result Result = NGX_DLSS_GET_OPTIMAL_SETTINGS(Parameters, DisplaySize.x, DisplaySize.y,
            QualityForMode(Mode, RequestedScale), &OptimalWidth, &OptimalHeight, &MaxWidth, &MaxHeight, &MinWidth, &MinHeight, &Sharpness);
        if (NVSDK_NGX_FAILED(Result) || OptimalWidth == 0)
        {
            return RequestedScale;
        }

        // A custom percentage keeps its own size inside the range the nearest mode accepts.
        if (Mode == EUpscalerMode::Custom && MinWidth > 0 && MaxWidth >= MinWidth)
        {
            const float Wanted = Math::Clamp(RequestedScale * (float)DisplaySize.x, (float)MinWidth, (float)MaxWidth);
            return Wanted / (float)DisplaySize.x;
        }
        return (float)OptimalWidth / (float)DisplaySize.x;
    }

    void FDLSSUpscaler::Evaluate(RHI::FCmdListH CL, const FUpscaleInputs& Inputs)
    {
        FScopeLock Lock(Mutex);
        if (!EnsureInitialized())
        {
            return;
        }

        VkCommandBuffer CommandBuffer = static_cast<VkCommandBuffer>(RHI::Native::GetNativeCommandBuffer(CL));
        if (CommandBuffer == VK_NULL_HANDLE)
        {
            return;
        }

        const float Scale = (float)Inputs.RenderSize.x / (float)Math::Max(Inputs.DisplaySize.x, 1u);
        const NVSDK_NGX_PerfQuality_Value Quality = QualityForMode(Inputs.Mode, Scale);

        if (Feature == nullptr || FeatureRender != Inputs.RenderSize || FeatureDisplay != Inputs.DisplaySize || FeatureQuality != (int32)Quality)
        {
            ReleaseFeature();

            NVSDK_NGX_DLSS_Create_Params Create = {};
            Create.Feature.InWidth            = Inputs.RenderSize.x;
            Create.Feature.InHeight           = Inputs.RenderSize.y;
            Create.Feature.InTargetWidth      = Inputs.DisplaySize.x;
            Create.Feature.InTargetHeight     = Inputs.DisplaySize.y;
            Create.Feature.InPerfQualityValue = Quality;
            // Motion vectors carry no jitter, depth is reverse-Z, and the color is pre-exposure linear HDR.
            Create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                                        | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

            const NVSDK_NGX_Result Result = NGX_VULKAN_CREATE_DLSS_EXT(CommandBuffer, 1, 1, &Feature, Parameters, &Create);
            if (NVSDK_NGX_FAILED(Result) || Feature == nullptr)
            {
                LOG_WARN("[DLSS] Creating the {}x{} to {}x{} feature failed ({:#x}).",
                    Inputs.RenderSize.x, Inputs.RenderSize.y, Inputs.DisplaySize.x, Inputs.DisplaySize.y, (uint32)Result);
                Feature = nullptr;
                return;
            }

            FeatureRender  = Inputs.RenderSize;
            FeatureDisplay = Inputs.DisplaySize;
            FeatureQuality = (int32)Quality;
            LOG_INFO("[DLSS] Feature created for {}x{} to {}x{}, quality mode {}.",
                Inputs.RenderSize.x, Inputs.RenderSize.y, Inputs.DisplaySize.x, Inputs.DisplaySize.y, (int32)Quality);
        }

        NVSDK_NGX_Resource_VK Color  = MakeResource(Inputs.Color,    false, false);
        NVSDK_NGX_Resource_VK Output = MakeResource(Inputs.Output,   false, true);
        NVSDK_NGX_Resource_VK Depth  = MakeResource(Inputs.Depth,    true,  false);
        NVSDK_NGX_Resource_VK Motion = MakeResource(Inputs.Velocity, false, false);

        NVSDK_NGX_VK_DLSS_Eval_Params Eval = {};
        Eval.Feature.pInColor               = &Color;
        Eval.Feature.pInOutput              = &Output;
        Eval.pInDepth                       = &Depth;
        Eval.pInMotionVectors               = &Motion;
        Eval.InJitterOffsetX                = Inputs.JitterPixels.x;
        Eval.InJitterOffsetY                = Inputs.JitterPixels.y;
        Eval.InRenderSubrectDimensions      = { Inputs.RenderSize.x, Inputs.RenderSize.y };
        Eval.InReset                        = Inputs.bReset ? 1 : 0;
        // The velocity image is in UV units, which the render size turns into the pixels DLSS reads.
        Eval.InMVScaleX                     = (float)Inputs.RenderSize.x;
        Eval.InMVScaleY                     = (float)Inputs.RenderSize.y;
        Eval.InPreExposure                  = 1.0f;
        Eval.InExposureScale                = 1.0f;
        Eval.InFrameTimeDeltaInMsec         = Inputs.DeltaSeconds * 1000.0f;

        const NVSDK_NGX_Result Result = NGX_VULKAN_EVALUATE_DLSS_EXT(CommandBuffer, Feature, Parameters, &Eval);
        if (NVSDK_NGX_FAILED(Result))
        {
            LOG_WARN("[DLSS] Evaluate failed ({:#x}).", (uint32)Result);
        }
    }

    void FDLSSUpscaler::ReleaseFeature()
    {
        if (Feature == nullptr)
        {
            return;
        }

        // A frame in flight may still be running the old feature.
        RHI::WaitDeviceIdle();
        NVSDK_NGX_VULKAN_ReleaseFeature(Feature);
        Feature        = nullptr;
        FeatureRender  = FUIntVector2(0);
        FeatureDisplay = FUIntVector2(0);
        FeatureQuality = -1;
    }

    void FDLSSUpscaler::ReleaseViewResources()
    {
        FScopeLock Lock(Mutex);
        ReleaseFeature();
    }

    void FDLSSUpscaler::Shutdown()
    {
        FScopeLock Lock(Mutex);
        if (InitializedDevice == nullptr)
        {
            return;
        }

        ReleaseFeature();
        if (Parameters != nullptr)
        {
            NVSDK_NGX_VULKAN_DestroyParameters(Parameters);
            Parameters = nullptr;
        }
        NVSDK_NGX_VULKAN_Shutdown1(static_cast<VkDevice>(InitializedDevice));
        InitializedDevice = nullptr;
        bAvailable = false;
    }
}
