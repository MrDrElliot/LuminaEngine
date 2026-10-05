#include "RenderCallbacks.h"
#include "RenderScene.h"
#include "Core/Profiler/Profile.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "Renderer/ShaderLibrary.h"

namespace Lumina
{
    RHI::FPipelineH FRenderContext::GetPipeline(const FRenderPipelineDesc& Desc) const
    {
        DEBUG_ASSERT(bPassOpen, "A pipeline without explicit targets takes them from the open pass.");

        // The engine's own passes write targets that cannot blend, such as the visibility buffer.
        const RHI::FBlendDesc Blend = bEnginePass ? RHI::FBlendDesc{} : Desc.Blend;

        TFixedVector<RHI::FColorTarget, 4> Targets;
        for (const EFormat Format : PassColorFormats)
        {
            Targets.push_back({ Format, Blend });
        }

        FRenderPipelineDesc Resolved = Desc;
        if (Resolved.PixelShader == nullptr)
        {
            Resolved.PixelShader = PassDefaultPixelShader;
        }

        return GetPipeline(Resolved, TSpan<const RHI::FColorTarget>(Targets.data(), Targets.size()), PassDepthFormat);
    }

    RHI::FPipelineH FRenderContext::GetPipeline(const FRenderPipelineDesc& Desc, TSpan<const RHI::FColorTarget> ColorTargets, EFormat DepthFormat) const
    {
        return Scene->GetCallbackPipeline(Desc, ColorTargets, DepthFormat);
    }

    RHI::FPipelineH FRenderContext::GetComputePipeline(FShaderH ComputeShader) const
    {
        return Scene->GetCallbackComputePipeline(ComputeShader);
    }

    void FRenderContext::BeginRenderPass(const FRenderTexture& Target, ESceneDepth Depth)
    {
        DEBUG_ASSERT(!bPassOpen, "End the open pass before beginning another.");
        DEBUG_ASSERT(Target.IsValid());

        const bool bUseDepth = Depth != ESceneDepth::None && SceneDepth.IsValid();
        DEBUG_ASSERT(!bUseDepth || SceneDepth.Extent == Target.Extent, "Scene depth only attaches to a target of the same size.");

        RHI::FRenderAttachment Color;
        Color.Texture = Target.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Target.Extent;
        if (bUseDepth)
        {
            Pass.DepthAttachment.Texture = SceneDepth.Texture;
            Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
            Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        }

        RHI::CmdBeginRenderPass(CmdList, Pass);

        const RHI::FRect Rect{ 0, (int)Target.Extent.x, 0, (int)Target.Extent.y };
        RHI::CmdSetViewport(CmdList, Rect);
        RHI::CmdSetScissor(CmdList, Rect);
        RHI::CmdSetCullMode(CmdList, RHI::ECullMode::None);

        RHI::FDepthStencilDesc DepthDesc;
        if (bUseDepth)
        {
            DepthDesc.DepthMode = Depth == ESceneDepth::TestAndWrite
                ? RHI::EDepthFlags::Read | RHI::EDepthFlags::Write
                : RHI::EDepthFlags::Read;
            DepthDesc.DepthTest = View.bReverseZ ? RHI::EOp::GreaterEqual : RHI::EOp::LessEqual;
        }
        RHI::CmdSetDepthStencil(CmdList, DepthDesc);

        PassColorFormats.clear();
        PassColorFormats.push_back(Target.Format);
        PassDepthFormat        = bUseDepth ? SceneDepth.Format : EFormat::UNKNOWN;
        PassDefaultPixelShader = {};
        bPassOpen              = true;
        bEnginePass            = false;
    }

    void FRenderContext::EndRenderPass()
    {
        DEBUG_ASSERT(bPassOpen && !bEnginePass, "Only a pass opened with BeginRenderPass can be ended here.");

        RHI::CmdEndRenderPass(CmdList);
        RHI::Barriers::RasterToRead(CmdList);

        PassColorFormats.clear();
        PassDepthFormat = EFormat::UNKNOWN;
        bPassOpen       = false;
    }

    void FRenderContext::Draw(const FRenderPipelineDesc& Desc, RHI::GPUPtr DrawArgs, uint32 VertexCount, uint32 InstanceCount) const
    {
        DEBUG_ASSERT(bPassOpen, "Draw needs an open pass.");

        const RHI::FPipelineH Pipeline = GetPipeline(Desc);
        if (!RHI::IsValid(Pipeline))
        {
            return;
        }

        RHI::CmdSetPipeline(CmdList, Pipeline);
        RHI::CmdDraw(CmdList, DrawArgs, VertexCount, InstanceCount, 0, 0);
    }

    void FRenderContext::DrawFullscreen(FShaderH PixelShader, RHI::GPUPtr DrawArgs, const FRenderTexture& Target, const RHI::FBlendDesc& Blend)
    {
        static const FShaderH FullscreenVertexShader = FShaderLibrary::Get("FullscreenQuad.slang");

        FRenderPipelineDesc Desc;
        Desc.VertexShader = FullscreenVertexShader;
        Desc.PixelShader  = PixelShader;
        Desc.Blend        = Blend;

        BeginRenderPass(Target, ESceneDepth::None);
        Draw(Desc, DrawArgs, 3);
        EndRenderPass();
    }

    void FRenderContext::Dispatch(FShaderH ComputeShader, RHI::GPUPtr DispatchArgs, uint32 GroupsX, uint32 GroupsY, uint32 GroupsZ) const
    {
        DEBUG_ASSERT(!bPassOpen, "Compute cannot run inside a render pass.");

        const RHI::FPipelineH Pipeline = GetComputePipeline(ComputeShader);
        if (!RHI::IsValid(Pipeline))
        {
            return;
        }

        RHI::CmdSetPipeline(CmdList, Pipeline);
        RHI::CmdDispatch(CmdList, DispatchArgs, GroupsX, GroupsY, GroupsZ);
        RHI::CmdBarrier(CmdList,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::AllCommands, RHI::EAccessFlags::Any);
    }

    void FRenderContext::AdoptEnginePass(TSpan<const EFormat> ColorFormats, EFormat DepthFormat, FShaderH DefaultPixelShader)
    {
        PassColorFormats.clear();
        for (const EFormat Format : ColorFormats)
        {
            PassColorFormats.push_back(Format);
        }
        PassDepthFormat        = DepthFormat;
        PassDefaultPixelShader = DefaultPixelShader;
        bPassOpen              = true;
        bEnginePass            = true;
    }

    FRenderCallbackHandle FRenderCallbackList::Add(ERenderStage Stages, FRenderCallback Callback, FName Name)
    {
        DEBUG_ASSERT(Threading::IsMainThread() || bInvoking);
        DEBUG_ASSERT(Stages != ERenderStage::None && Callback);

        FEntry Entry;
        Entry.Id       = NextId++;
        Entry.Stages   = Stages;
        Entry.Name     = Name;
        Entry.Callback = Move(Callback);

        const FRenderCallbackHandle Handle{ Entry.Id };

        // Entries must not grow while Invoke walks them, so an add from a callback lands afterwards.
        if (bInvoking)
        {
            PendingAdds.push_back(Move(Entry));
        }
        else
        {
            Entries.push_back(Move(Entry));
            ActiveStages |= Stages;
        }

        return Handle;
    }

    void FRenderCallbackList::Remove(FRenderCallbackHandle& Handle)
    {
        DEBUG_ASSERT(Threading::IsMainThread() || bInvoking);

        if (!Handle.IsValid())
        {
            return;
        }

        const uint64 Id = Handle.Id;
        Handle = {};

        for (SIZE_T i = 0; i < PendingAdds.size(); ++i)
        {
            if (PendingAdds[i].Id == Id)
            {
                PendingAdds.erase(PendingAdds.begin() + i);
                return;
            }
        }

        for (SIZE_T i = 0; i < Entries.size(); ++i)
        {
            if (Entries[i].Id != Id)
            {
                continue;
            }

            // Erasing would shift the entry Invoke is running, so it is skipped and swept afterwards.
            if (bInvoking)
            {
                Entries[i].bRemoved = true;
                bHasRemoved = true;
            }
            else
            {
                Entries.erase(Entries.begin() + i);
                RefreshActiveStages();
            }
            return;
        }
    }

    void FRenderCallbackList::Invoke(FRenderContext& Context)
    {
        if (!HasAny(Context.Stage))
        {
            return;
        }

        bInvoking = true;

        for (SIZE_T i = 0; i < Entries.size(); ++i)
        {
            if (Entries[i].bRemoved || !EnumHasAnyFlags(Entries[i].Stages, Context.Stage))
            {
                continue;
            }

            const char* Label = Entries[i].Name.c_str();
            LUMINA_PROFILE_SECTION_DYNAMIC(Label);
            const bool bMarker = RHI::IsValid(Context.CmdList);
            if (bMarker)
            {
                RHI::CmdBeginMarker(Context.CmdList, Label);
            }

            Entries[i].Callback(Context);

            // A pass left open would swallow every engine pass after it.
            if (Context.bPassOpen && !Context.bEnginePass)
            {
                LOG_ERROR_ONCE("Render callback '{}' returned with its render pass still open; closing it.", Entries[i].Name.ToString());
                Context.EndRenderPass();
            }

            if (bMarker)
            {
                RHI::CmdEndMarker(Context.CmdList);
            }
        }

        bInvoking = false;
        ApplyDeferredChanges();
    }

    void FRenderCallbackList::ApplyDeferredChanges()
    {
        if (bHasRemoved)
        {
            bHasRemoved = false;
            for (SIZE_T i = Entries.size(); i-- > 0;)
            {
                if (Entries[i].bRemoved)
                {
                    Entries.erase(Entries.begin() + i);
                }
            }
        }

        for (FEntry& Entry : PendingAdds)
        {
            Entries.push_back(Move(Entry));
        }
        PendingAdds.clear();

        RefreshActiveStages();
    }

    void FRenderCallbackList::RefreshActiveStages()
    {
        ActiveStages = ERenderStage::None;
        for (const FEntry& Entry : Entries)
        {
            ActiveStages |= Entry.Stages;
        }
    }
}
