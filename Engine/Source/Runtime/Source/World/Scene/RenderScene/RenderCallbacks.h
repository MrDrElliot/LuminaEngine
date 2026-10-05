#pragma once

#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/LuminaMacros.h"
#include "Core/Math/Math.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Renderer/ShaderHandle.h"

namespace Lumina
{
    class CWorld;
    class IRenderScene;

    // Where in a world's frame a render callback runs. One callback may register for several stages.
    enum class ERenderStage : uint16
    {
        None              = 0,

        // Before any scene geometry, for simulations whose results the frame then samples.
        FrameStart        = BIT(0),

        // Inside the engine's depth pass before occlusion culling, so custom geometry occludes the scene.
        Depth             = BIT(1),

        // Inside each shadow map pass, once per cascade, spot light and point light face.
        ShadowDepth       = BIT(2),

        // Opaque lighting and depth are complete, and fog, water and translucency have not run.
        AfterOpaque       = BIT(3),

        // The full HDR scene, before depth of field, bloom and exposure.
        AfterTranslucency = BIT(4),

        // Tone mapped and post processed display color, before anti-aliasing.
        AfterPostProcess  = BIT(5),

        // The final image, after anti-aliasing and before the UI.
        Overlay           = BIT(6),
    };
    ENUM_CLASS_FLAGS(ERenderStage);

    enum class EShadowViewType : uint8
    {
        None,
        Cascade,
        Spot,
        PointFace,
    };

    // How a pass opened with FRenderContext::BeginRenderPass treats scene depth.
    enum class ESceneDepth : uint8
    {
        None,
        Test,
        TestAndWrite,
    };

    struct FRenderTexture
    {
        RHI::FTextureH Texture;
        uint32         Index  = ~0u;
        EFormat        Format = EFormat::UNKNOWN;
        FUIntVector2   Extent = FUIntVector2(0);

        bool IsValid() const { return RHI::IsValid(Texture); }
    };

    // The view being drawn into, which is the light's view during ShadowDepth.
    struct FRenderView
    {
        FMatrix4     ViewProjection        = FMatrix4(1.0f);
        FMatrix4     InverseViewProjection = FMatrix4(1.0f);
        FUIntVector2 Extent                = FUIntVector2(0);
        bool         bReverseZ             = true;
    };

    // The player camera, in every stage.
    struct FRenderCamera
    {
        FMatrix4 View              = FMatrix4(1.0f);
        FMatrix4 InverseView       = FMatrix4(1.0f);
        FMatrix4 Projection        = FMatrix4(1.0f);
        FMatrix4 InverseProjection = FMatrix4(1.0f);
        FVector3 Location          = FVector3(0.0f);
        float    NearPlane         = 0.0f;
        float    FarPlane          = 0.0f;
    };

    struct FShadowView
    {
        EShadowViewType Type  = EShadowViewType::None;
        uint32          Index = 0;
    };

    struct FRenderPipelineDesc
    {
        FShaderH        VertexShader;
        FShaderH        PixelShader;
        FShaderH        MeshShader;
        RHI::ETopology  Topology = RHI::ETopology::TriangleList;
        RHI::FBlendDesc Blend;
    };

    class RUNTIME_API FRenderContext
    {
    public:

        explicit FRenderContext(IRenderScene& InScene) : Scene(&InScene) {}

        RHI::FCmdListH  CmdList;
        CWorld*         World     = nullptr;
        ERenderStage    Stage     = ERenderStage::None;
        uint32          FrameSlot = 0;
        float           Time      = 0.0f;
        float           DeltaTime = 0.0f;

        FRenderView     View;
        FRenderCamera   Camera;
        FShadowView     Shadow;

        FRenderTexture  SceneColor;
        FRenderTexture  SceneDepth;
        FRenderTexture  Velocity;

        // Uploads Value for this frame only, which is how shaders receive their pass arguments.
        template<typename T>
        RHI::GPUPtr Args(const T& Value) const { return RHI::CopyTransient(Value); }

        // Targets come from the open pass. Cached, and rebuilt when a shader hot reloads.
        RHI::FPipelineH GetPipeline(const FRenderPipelineDesc& Desc) const;
        RHI::FPipelineH GetPipeline(const FRenderPipelineDesc& Desc, TSpan<const RHI::FColorTarget> ColorTargets, EFormat DepthFormat) const;
        RHI::FPipelineH GetComputePipeline(FShaderH ComputeShader) const;

        void BeginRenderPass(const FRenderTexture& Target, ESceneDepth Depth = ESceneDepth::Test);
        void EndRenderPass();
        bool IsInRenderPass() const { return bPassOpen; }

        void Draw(const FRenderPipelineDesc& Desc, RHI::GPUPtr DrawArgs, uint32 VertexCount, uint32 InstanceCount = 1) const;

        template<typename T>
        void Draw(const FRenderPipelineDesc& Desc, const T& DrawArgs, uint32 VertexCount, uint32 InstanceCount = 1) const
        {
            Draw(Desc, Args(DrawArgs), VertexCount, InstanceCount);
        }

        // A full-screen triangle into Target, whose pixel shader receives the UV at TEXCOORD0.
        void DrawFullscreen(FShaderH PixelShader, RHI::GPUPtr DrawArgs, const FRenderTexture& Target, const RHI::FBlendDesc& Blend = {});

        template<typename T>
        void DrawFullscreen(FShaderH PixelShader, const T& DrawArgs, const FRenderTexture& Target, const RHI::FBlendDesc& Blend = {})
        {
            DrawFullscreen(PixelShader, Args(DrawArgs), Target, Blend);
        }

        // Its writes are visible to everything recorded after it.
        void Dispatch(FShaderH ComputeShader, RHI::GPUPtr DispatchArgs, uint32 GroupsX, uint32 GroupsY = 1, uint32 GroupsZ = 1) const;

        template<typename T>
        void Dispatch(FShaderH ComputeShader, const T& DispatchArgs, uint32 GroupsX, uint32 GroupsY = 1, uint32 GroupsZ = 1) const
        {
            Dispatch(ComputeShader, Args(DispatchArgs), GroupsX, GroupsY, GroupsZ);
        }

        // For renderers, which describe the pass they opened before running Depth and ShadowDepth callbacks.
        void AdoptEnginePass(TSpan<const EFormat> ColorFormats, EFormat DepthFormat, FShaderH DefaultPixelShader);

    private:

        friend class FRenderCallbackList;

        IRenderScene*            Scene = nullptr;
        TFixedVector<EFormat, 4> PassColorFormats;
        EFormat                  PassDepthFormat        = EFormat::UNKNOWN;
        FShaderH                 PassDefaultPixelShader;
        bool                     bPassOpen              = false;
        bool                     bEnginePass            = false;
    };

    using FRenderCallback = TFunction<void(FRenderContext&)>;

    struct FRenderCallbackHandle
    {
        uint64 Id = 0;

        bool IsValid() const { return Id != 0; }
    };

    // A world's render callbacks. Add and remove on the game thread or from inside a callback.
    class RUNTIME_API FRenderCallbackList
    {
    public:

        FRenderCallbackHandle Add(ERenderStage Stages, FRenderCallback Callback, FName Name);

        // Safe from inside a callback, and resets Handle.
        void Remove(FRenderCallbackHandle& Handle);

        bool HasAny(ERenderStage Stage) const { return EnumHasAnyFlags(ActiveStages, Stage); }

        // Runs every callback registered for Context.Stage, in the order they were added.
        void Invoke(FRenderContext& Context);

    private:

        struct FEntry
        {
            uint64          Id = 0;
            ERenderStage    Stages = ERenderStage::None;
            FName           Name;
            FRenderCallback Callback;
            bool            bRemoved = false;
        };

        void ApplyDeferredChanges();
        void RefreshActiveStages();

        TVector<FEntry> Entries;
        TVector<FEntry> PendingAdds;
        uint64          NextId       = 1;
        ERenderStage    ActiveStages = ERenderStage::None;
        bool            bInvoking    = false;
        bool            bHasRemoved  = false;
    };
}
