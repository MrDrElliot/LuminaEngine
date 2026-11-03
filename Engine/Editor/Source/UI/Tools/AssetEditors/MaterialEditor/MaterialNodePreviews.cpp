#include "MaterialNodePreviews.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Core/Object/Cast.h"
#include "Core/Math/Packing.h"
#include "World/Scene/RenderScene/Default/DefaultSceneRenderer.h"
#include "Thumbnails/ThumbnailManager.h"
#include "Thumbnails/ThumbnailScene.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialGraphNode.h"

namespace Lumina
{
    namespace
    {
        constexpr uint32 kPreviewRenderSize    = 192;
        constexpr int32  kMaxPreviewsCompiling = 2;

        // The renderer draws a stand-in until the new pipelines finish building, so a compile is captured more than once.
        constexpr int32  kCapturesPerCompile   = 3;
        constexpr double kRecaptureSeconds     = 0.75;
    }

    bool FPinPreviewMaterial::Launch(CMaterialNodeGraph* Graph, uint32 InPinID)
    {
        CMaterialOutput* Pin = Cast<CMaterialOutput>(Graph->FindPinByGUID(InPinID));
        if (Pin == nullptr || Job != nullptr)
        {
            return false;
        }

        if (!Material.IsValid())
        {
            Material = NewObject<CMaterial>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            Material->ShadingModel = EMaterialShadingModel::Unlit;
        }

        PinID   = InPinID;
        Version = Graph->GetContentVersion();
        Job     = MakeUnique<FJob>();
        Job->Compiler.SetPreviewOutput(Pin);
        if (BeginMaterialGraphCompile(Material.Get(), Graph, Job->Compiler, Job->Result, {}, ETaskPriority::Low))
        {
            return true;
        }

        Error  = Job->Result.Errors.empty() ? FString("That pin has nothing to compile.") : Job->Result.Errors[0].Description;
        bReady = false;
        Job.reset();
        return false;
    }

    bool FPinPreviewMaterial::Poll()
    {
        if (Job == nullptr || Job->Result.StageLog == nullptr || !Job->Result.StageLog->Stages.IsSettled())
        {
            return false;
        }

        FinishMaterialGraphCompile(Material.Get(), Job->Compiler, Job->Result);
        bReady = Job->Result.bSuccess;
        Error  = bReady || Job->Result.Errors.empty() ? FString() : Job->Result.Errors[0].Description;
        Job.reset();
        return true;
    }

    FViewportPixelProbe::~FViewportPixelProbe()
    {
        for (FSlot& Slot : Slots)
        {
            if (Slot.Readback.Gpu != 0)
            {
                RHI::Retire(Slot.Readback);
            }
        }
    }

    void FViewportPixelProbe::Tick(IRenderScene* Scene, bool bWanted, ImVec2 UV)
    {
        for (FSlot& Slot : Slots)
        {
            if (Slot.bPending && RHI::GetSemaphoreValue(Slot.Semaphore) >= Slot.Value)
            {
                Slot.bPending = false;
                if (const uint16* Texel = Slot.Readback.CpuAs<const uint16>())
                {
                    Latest     = ImVec4(Math::Detail::HalfToFloat(Texel[0]), Math::Detail::HalfToFloat(Texel[1]),
                                        Math::Detail::HalfToFloat(Texel[2]), Math::Detail::HalfToFloat(Texel[3]));
                    bHasLatest = true;
                }
            }
        }

        if (!bWanted || Scene == nullptr)
        {
            bHasLatest = false;
            return;
        }

        FSlot& Slot = Slots[NextSlot];
        const FSceneImage& HDR = static_cast<FDefaultSceneRenderer*>(Scene)->GetPrimaryNamedImage(FDefaultSceneRenderer::ENamedImage::HDR);
        if (Slot.bPending || !HDR.IsValid() || HDR.Desc.Format != EFormat::RGBA16_FLOAT)
        {
            return;
        }

        if (Slot.Readback.Gpu == 0)
        {
            Slot.Readback = RHI::Malloc(sizeof(uint16) * 4, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            RHI::SetDebugName(Slot.Readback.Gpu, "Readback.MaterialProbe");
        }

        RHI::FTextureSlice Texel;
        Texel.Offset = FUIntVector3((uint32)Math::Clamp(UV.x * (float)HDR.GetSizeX(), 0.0f, (float)HDR.GetSizeX() - 1.0f),
                                    (uint32)Math::Clamp(UV.y * (float)HDR.GetSizeY(), 0.0f, (float)HDR.GetSizeY() - 1.0f), 0);
        Texel.Extent = FUIntVector3(1, 1, 1);

        RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute | RHI::EStageFlags::Transfer,
            RHI::EAccessFlags::ColorWrite | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead);
        RHI::CmdCopyTextureToMemory(CL, HDR.Texture, Texel, Slot.Readback, 1);
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite, RHI::EStageFlags::Host, RHI::EAccessFlags::HostRead);

        Slot.Semaphore = RHI::GetQueueTimeline(RHI::EQueueType::Graphics);
        Slot.Value     = RHI::Submit(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{&CL, 1});
        Slot.bPending  = true;
        NextSlot       = (NextSlot + 1) % (uint32)std::size(Slots);
    }

    FMaterialNodePreviews::FMaterialNodePreviews() = default;
    FMaterialNodePreviews::~FMaterialNodePreviews() = default;

    FMaterialNodePreviews::FNodePreview::~FNodePreview()
    {
        if (Texture.IsValid())
        {
            RHI::Textures::Release(Texture);
        }
    }

    void FMaterialNodePreviews::Tick(CMaterialNodeGraph* Graph)
    {
        erase_if(Previews, [Graph](const auto& Pair)
        {
            const CMaterialGraphNode* Node = Cast<CMaterialGraphNode>(Graph->FindNode(Pair.first));
            return Node == nullptr || !Node->bShowPreview;
        });

        int32 Compiling = 0;
        for (const TStrongObjectPtr<CEdGraphNode>& NodeRef : Graph->Nodes)
        {
            const CMaterialGraphNode* Node = Cast<CMaterialGraphNode>(NodeRef.Get());
            if (Node != nullptr && Node->bShowPreview && !Node->GetOutputPins().empty() && !Previews.contains(Node->GetNodeID()))
            {
                Previews.emplace(Node->GetNodeID(), MakeUnique<FNodePreview>());
            }
        }

        for (auto& [NodeID, Preview] : Previews)
        {
            if (Preview->Material.Poll() && Preview->Material.IsReady())
            {
                Preview->CapturesLeft    = kCapturesPerCompile;
                Preview->NextCaptureTime = 0.0;
            }
            Compiling += Preview->Material.IsCompiling() ? 1 : 0;
        }

        // A handful at a time, since each compile builds every stage of a surface material.
        const uint64 Version = Graph->GetContentVersion();
        for (auto& [NodeID, Preview] : Previews)
        {
            if (Compiling >= kMaxPreviewsCompiling || Preview->Material.IsCompiling() || Preview->Material.GetVersion() == Version)
            {
                continue;
            }

            const CEdGraphNode* Node = Graph->FindNode(NodeID);
            if (Node == nullptr || Node->GetOutputPins().empty())
            {
                continue;
            }
            if (Preview->Material.Launch(Graph, Node->GetOutputPins()[0]->GetPinGUID()))
            {
                ++Compiling;
            }
        }

        if (CapturingNode != 0)
        {
            FinishCapture();
            return;
        }

        const double Now = ImGui::GetTime();
        for (auto& [NodeID, Preview] : Previews)
        {
            if (Preview->CapturesLeft <= 0 || Now < Preview->NextCaptureTime)
            {
                continue;
            }

            if (Scene == nullptr)
            {
                Scene = MakeUnique<FThumbnailScene>(kPreviewRenderSize);
                Scene->Begin();
            }

            --Preview->CapturesLeft;
            Preview->NextCaptureTime = Now + kRecaptureSeconds * (double)(kCapturesPerCompile - Preview->CapturesLeft);
            if (CThumbnailManager::Get().StageThumbnail(*Scene, Preview->Material.GetMaterial()) && Scene->BeginCapture())
            {
                CapturingNode = NodeID;
            }
            break;
        }
    }

    void FMaterialNodePreviews::FinishCapture()
    {
        if (!Scene->IsCaptureReady())
        {
            return;
        }

        FPackageThumbnail Captured;
        const bool bCaptured = Scene->FinishCapture(Captured);
        auto Found = Previews.find(CapturingNode);
        CapturingNode = 0;
        if (!bCaptured || Found == Previews.end())
        {
            return;
        }

        if (Found->second->Texture.IsValid())
        {
            RHI::Textures::Release(Found->second->Texture);
        }
        Found->second->Texture = CThumbnailManager::CreateThumbnailTexture(Captured, "MaterialNodePreview");
    }

    bool FMaterialNodePreviews::GetTexture(int64 NodeID, ImTextureRef& OutTexture, const char*& OutStatus) const
    {
        auto Found = Previews.find(NodeID);
        if (Found == Previews.end())
        {
            OutStatus = "Compiling";
            return false;
        }

        const FNodePreview& Preview = *Found->second;
        if (Preview.Texture.IsValid())
        {
            OutTexture = ImGuiX::ToImTextureRef(Preview.Texture);
            return true;
        }

        OutStatus = !Preview.Material.IsCompiling() && !Preview.Material.GetError().empty() ? "No preview" : "Compiling";
        return false;
    }
}
