#pragma once

#include "imgui.h"
#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "Renderer/RHI.h"
#include "Renderer/RHITexture.h"
#include "UI/Tools/NodeGraph/Material/MaterialGraphCompile.h"

namespace Lumina
{
    class CMaterial;
    class CMaterialNodeGraph;
    class FThumbnailScene;
    class IRenderScene;

    // One output pin compiled on its own into a transient unlit material, in the background.
    class FPinPreviewMaterial
    {
    public:

        // False when a compile is already running or the pin cannot be compiled, with the reason in GetError.
        bool Launch(CMaterialNodeGraph* Graph, uint32 InPinID);

        // True on the frame a compile lands, after which IsReady says whether it worked.
        bool Poll();

        bool           IsCompiling() const { return Job != nullptr; }
        bool           IsReady() const { return bReady; }
        CMaterial*     GetMaterial() const { return Material.Get(); }
        uint32         GetPinID() const { return PinID; }
        uint64         GetVersion() const { return Version; }
        const FString& GetError() const { return Error; }

    private:

        struct FJob
        {
            FMaterialCompiler           Compiler;
            FMaterialGraphCompileResult Result;
        };

        TStrongObjectPtr<CMaterial> Material;
        TUniquePtr<FJob>            Job;
        uint32                      PinID   = 0;
        uint64                      Version = ~0ull;
        bool                        bReady  = false;
        FString                     Error;
    };

    // Reads the HDR scene color under the cursor a few frames after asking, never waiting on the GPU.
    class FViewportPixelProbe
    {
    public:

        FViewportPixelProbe() = default;
        ~FViewportPixelProbe();
        FViewportPixelProbe(const FViewportPixelProbe&) = delete;
        FViewportPixelProbe& operator=(const FViewportPixelProbe&) = delete;
        FViewportPixelProbe(FViewportPixelProbe&&) = delete;
        FViewportPixelProbe& operator=(FViewportPixelProbe&&) = delete;

        // UV is the cursor across the viewport image, 0 to 1 each way.
        void Tick(IRenderScene* Scene, bool bWanted, ImVec2 UV);
        bool GetValue(ImVec4& OutValue) const { OutValue = Latest; return bHasLatest; }

    private:

        struct FSlot
        {
            RHI::FGPUAllocation Readback  = {};
            RHI::FSemaphoreH    Semaphore = {};
            uint64              Value     = 0;
            bool                bPending  = false;
        };

        FSlot  Slots[RHI::kFramesInFlight + 1];
        uint32 NextSlot   = 0;
        ImVec4 Latest;
        bool   bHasLatest = false;
    };

    // Renders the nodes whose preview is on, each compiled in the background and captured one at a time.
    class FMaterialNodePreviews
    {
    public:

        FMaterialNodePreviews();
        ~FMaterialNodePreviews();
        FMaterialNodePreviews(const FMaterialNodePreviews&) = delete;
        FMaterialNodePreviews& operator=(const FMaterialNodePreviews&) = delete;
        FMaterialNodePreviews(FMaterialNodePreviews&&) = delete;
        FMaterialNodePreviews& operator=(FMaterialNodePreviews&&) = delete;

        void Tick(CMaterialNodeGraph* Graph);

        // The node's latest capture, or false with what is happening instead.
        bool GetTexture(int64 NodeID, ImTextureRef& OutTexture, const char*& OutStatus) const;

    private:

        struct FNodePreview
        {
            FPinPreviewMaterial  Material;
            RHI::FManagedTexture Texture;
            double               NextCaptureTime = 0.0;
            int32                CapturesLeft    = 0;

            FNodePreview() = default;
            ~FNodePreview();
            FNodePreview(const FNodePreview&) = delete;
            FNodePreview& operator=(const FNodePreview&) = delete;
            FNodePreview(FNodePreview&&) = delete;
            FNodePreview& operator=(FNodePreview&&) = delete;
        };

        void FinishCapture();

        TUniquePtr<FThumbnailScene>               Scene;
        THashMap<int64, TUniquePtr<FNodePreview>> Previews;
        int64                                     CapturingNode = 0;
    };
}
