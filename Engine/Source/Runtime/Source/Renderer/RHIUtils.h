#pragma once

#include "RHI.h"
#include "RHITexture.h"

namespace Lumina::RHI::Utils
{
    struct FScreenPassDesc
    {
        FTextureH      Target;
        FUIntVector2   Extent;
        FDepthStencilDesc DepthState;
        ELoadOp        LoadOp   = ELoadOp::Clear;
        float          Clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    };

    // Opens a color only pass and sets the state every screen space pass wants anyway.
    RUNTIME_API void BeginScreenPass(FCmdListH CL, const FScreenPassDesc& Desc);
    RUNTIME_API void EndScreenPass(FCmdListH CL);

    // Three vertices, no vertex buffer. Pairs with a fullscreen triangle vertex shader.
    RUNTIME_API void DrawFullscreen(FCmdListH CL, FPipelineH Pipeline, GPUPtr Args);

    // Half resolution color chain, which is the shape a bloom or blur pyramid needs.
    class RUNTIME_API FMipChain
    {
    public:

        static constexpr uint32 kMaxLevels = 12;

        void Initialize(const FUIntVector2& BaseExtent, uint32 LevelCount, EFormat Format, const char* DebugName);
        void Shutdown();

        NODISCARD uint32 Num() const { return Count; }
        NODISCARD bool IsValid() const { return Count > 0; }

        NODISCARD FTextureH Texture(uint32 Level) const { return Levels[Level].Texture; }
        NODISCARD uint32 SampledSlot(uint32 Level) const { return Levels[Level].SampledSlot; }
        NODISCARD FUIntVector2 Extent(uint32 Level) const { return Extents[Level]; }

        NODISCARD FVector2 TexelSize(uint32 Level) const
        {
            return { 1.0f / float(Extents[Level].x), 1.0f / float(Extents[Level].y) };
        }

    private:

        FManagedTexture Levels[kMaxLevels];
        FUIntVector2    Extents[kMaxLevels] {};
        uint32          Count = 0;
    };
}
