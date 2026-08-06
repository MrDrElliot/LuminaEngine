#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "UI/ColorTextEdit/TextEditor.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

namespace Lumina
{
    class CEdGraphNode;
}

namespace Lumina
{
    class CMaterialNodeGraph;

    class FMaterialEditorTool : public FAssetEditorTool
    {
    public:

        struct FCompilationError
        {
            FString             Title;
            FString             Description;
            CEdGraphNode*       Node = nullptr;
        };

        struct FCompilationResultInfo
        {
            FString                     CompilationLog;
            TVector<FCompilationError>  Errors;
            bool                        bIsError = false;
        };
        
        enum class EDebugMesh : uint8
        {
            Sphere,
            Cube,
            Plane,
            Cylinder,
            Cone,
        };

        LUMINA_EDITOR_TOOL(FMaterialEditorTool)

        FMaterialEditorTool(IEditorToolContext* Context, CObject* InAsset);
        
        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_SPHERE; }
        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void SetupWorldForTool() override;

        bool DrawViewport(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture) override;
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        bool ShouldGenerateThumbnailOnSave() const override { return true; }
        void OnAssetLoadFinished() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawMaterialGraph();
        void DrawMaterialProperties();
        void DrawShaderStats();
        // Compiled-shader truth (local-memory arrays + the driver's register count / occupancy), as
        // opposed to the source-derived estimates the rest of the panel shows.
        void DrawGPUStats(float LabelWidth, const ImVec4& HeaderColor, const ImVec4& LabelColor, const ImVec4& ValueColor);

        void Compile();
        void ApplyMaterialToPreview();
        void FocusGraphNode(CEdGraphNode* Node);

        // Syntax-highlighted editor for the selected Custom Slang node's body. Bound lazily to the
        // selection; edits write straight back to the node (auto-compile picks them up).
        void DrawCustomCodeEditor();
        void OnSave() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    protected:
        
        void DrawHelpMenu() override;
        
    private:
        
        void SetDebugMesh(EDebugMesh Mesh, FStringView Path = "");
        
    private:
        
        entt::entity                    MeshEntity;
        entt::entity                    DirectionalLightEntity;
        
        FString                         Tree;
        FString                         VertexTree;
        // True when the graph has changed since the last successful Compile, so the Compile action can
        // say so instead of leaving the user to notice their edits never reached the shader.
        NODISCARD bool NeedsCompile() const;

        FMaterialCompiler::FShaderStats ShaderStats;
        bool                            bHasCompiledOnce = false;

        // Graph content version captured at the last Compile; compared against the live one by
        // NeedsCompile. Property edits bump the graph's version too (see the post-edit callback).
        uint64                          CompiledContentVersion = 0;
        size_t                          ReplacementStart = 0;
        size_t                          ReplacementEnd = 0;
        CEdGraphNode*                   SelectedNode = nullptr;

        // Custom Slang code editor state. CodeEditorBoundNode tracks which node the buffer currently
        // holds, so switching selection reloads instead of writing one node's text into another.
        TextEditor                      CodeEditor;
        CEdGraphNode*                   CodeEditorBoundNode = nullptr;
        // Undo cursor at the last write-back; a change means the user actually edited the buffer,
        // so we don't rewrite (and dirty) the package every frame.
        size_t                          LastCodeEditorUndoIndex = 0;
        FCompilationResultInfo          CompilationResult;
        
        
        TUniquePtr<FPropertyTable>      EnvironmentEditor;
        TUniquePtr<FPropertyTable>      DirectionalEditor;
        
        TObjectPtr<CMaterialNodeGraph>  NodeGraph;
        bool                            bGLSLPreviewDirty = false;
        
        EDebugMesh                      DebugMesh;
    };
}
