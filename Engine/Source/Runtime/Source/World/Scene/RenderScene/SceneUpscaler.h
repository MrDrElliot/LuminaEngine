#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "RenderCallbacks.h"
#include "Lumina.h"

namespace Lumina
{
    enum class EUpscalerMode : uint8;

    // One frame of a view for an upscaler. Everything is at render resolution except Output.
    struct FUpscaleInputs
    {
        // Linear HDR scene color before exposure and tonemapping.
        FRenderTexture Color;

        // Reverse-Z device depth, 1 at the near plane and 0 at the far plane.
        FRenderTexture Depth;

        // Previous UV minus current UV per pixel, so multiplying by RenderSize gives pixels toward the last frame.
        FRenderTexture Velocity;

        // A 1x1 R32F of the adapted scene luminance the tonemapper will expose against.
        FRenderTexture AdaptedLuminance;

        // Display resolution RGBA16F with storage and color attachment use, which the upscaler fills completely.
        FRenderTexture Output;

        FUIntVector2 RenderSize  = FUIntVector2(0);
        FUIntVector2 DisplaySize = FUIntVector2(0);

        // The quality mode the settings chose, which an upscaler with modes of its own keys its setup on.
        EUpscalerMode Mode{};

        // This frame's projection offset in render pixels, in the [-0.5, 0.5] range with +Y down.
        FVector2 JitterPixels = FVector2(0.0f);

        float NearPlane         = 0.0f;
        float FarPlane          = 0.0f;
        float VerticalFovRadians = 0.0f;
        float DeltaSeconds      = 0.0f;

        // History is meaningless this frame, after a camera cut, a resize or the first frame of a view.
        bool bReset = false;
    };

    // A temporal or spatial upscaler such as DLSS, chosen by CRendererSettings::Upscaler.
    class IUpscaler
    {
    public:

        virtual ~IUpscaler() = default;

        virtual FName GetName() const = 0;

        // False lets the renderer fall back to its spatial upscale, such as on a GPU the SDK does not support.
        virtual bool IsSupported() const = 0;

        // A temporal upscaler gets a jittered projection and motion vectors, and replaces SMAA.
        virtual bool IsTemporal() const = 0;

        // The fraction of the display each axis renders at for a mode, where RequestedScale is the engine's standard ratio for it.
        virtual float GetRenderScale(const FUIntVector2& DisplaySize, EUpscalerMode Mode, float RequestedScale) const { return RequestedScale; }

        // How many jitter positions one cycle visits, which DLSS wants to grow with the upscale ratio.
        RUNTIME_API virtual uint32 GetJitterPhaseCount(const FUIntVector2& RenderSize, const FUIntVector2& DisplaySize) const;

        // Records into CL. Every input is readable and Output writable, and Output must be complete when it returns.
        virtual void Evaluate(RHI::FCmdListH CL, const FUpscaleInputs& Inputs) = 0;

        // The view it was evaluating for went away, so per-view state such as a feature handle can be freed.
        virtual void ReleaseViewResources() {}
    };

    struct RUNTIME_API FUpscalerRegistry
    {
        static void Register(IUpscaler* Upscaler);
        static void Unregister(IUpscaler* Upscaler);

        static IUpscaler* Find(const FName& Name);

        // The one CRendererSettings::Upscaler names, or null for the built-in spatial upscale.
        static IUpscaler* GetActive();

        static void GetRegistered(TVector<IUpscaler*>& Out);
    };

    namespace Upscaling
    {
        struct FUpscaleRequest
        {
            EUpscalerMode Mode{};
            float         Scale = 1.0f;
        };

        // What the settings ask for, where r.ScreenPercentage above zero forces a custom percentage.
        RUNTIME_API FUpscaleRequest GetRequest();

        // The engine's per-axis ratio for a mode, CustomScale being the one Custom uses.
        RUNTIME_API float GetModeScale(EUpscalerMode Mode, float CustomScale);

        // The Halton (2, 3) point for Index, centered on the pixel, which is the sequence DLSS and FSR document.
        RUNTIME_API FVector2 HaltonJitter(uint32 Index, uint32 PhaseCount);

        // Each axis scaled and rounded, never below one pixel and never above the display.
        RUNTIME_API FUIntVector2 ScaleExtent(const FUIntVector2& DisplaySize, float Scale);
    }
}
