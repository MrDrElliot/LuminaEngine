#pragma once

#include "Audio/AudioTypes.h"
#include "Audio/Graph/AudioGraphTypes.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

namespace Lumina
{
    class CAudioNodeGraph;
    class CEdGraphNode;
    class FAudioGraphInstance;

    /** Editor for CAudioGraph, offering the node canvas, a properties panel and an audition transport. */
    class FAudioGraphEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FAudioGraphEditorTool)

        FAudioGraphEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        bool ShouldGenerateThumbnailOnSave() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_WAVEFORM; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void Update(const FUpdateContext& UpdateContext) override;

        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void OnSave() override;

    private:

        void DrawGraphWindow();
        void DrawPropertiesWindow();
        void DrawTransportWindow();

        /** Flattens the canvas into the asset's program. Returns false when the graph has errors. */
        bool Compile(bool bMarkPackageDirty = true);

        void StartPreview();
        void StopPreview();

        TObjectPtr<CAudioNodeGraph>     NodeGraph;
        CEdGraphNode*                   SelectedNode = nullptr;

        /** Live value behind each transport slider, since the instance owns no readable copy. */
        struct FPreviewParameter
        {
            FName           Name;
            EAudioGraphType Type = EAudioGraphType::Float;
            float           FloatValue = 0.0f;
            int32           IntValue = 0;
            bool            BoolValue = false;
        };

        TSharedPtr<FAudioGraphInstance> PreviewInstance;
        FAudioHandle                    PreviewHandle;
        TVector<FPreviewParameter>      PreviewParameters;

        TVector<FString>                CompileMessages;
        bool                            bHasErrors = false;

        /** Content version the last compile ran at, so an idle graph is not recompiled every frame. */
        uint64                          CompiledContentVersion = 0;
        bool                            bHasCompiledOnce = false;
    };
}
