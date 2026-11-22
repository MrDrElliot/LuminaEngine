#pragma once

#include "MaterialNodePreviews.h"
#include "World/ECS/Registry.h"


#include "Core/Object/ObjectHandleTyped.h"
#include "UI/CodeEditor/CodeEditor.h"
#include "UI/CodeEditor/Languages/SlangLanguage.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

namespace Lumina
{
    class CEdGraphNode;
    class CMaterialExpression_CustomSlang;
    class CMaterialInterface;
    class CParticleSystem;
}

namespace Lumina
{
    class CMaterialNodeGraph;

    class EDITOR_API FMaterialEditorTool : public FAssetEditorTool
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
            // Non-fatal findings that share the error payload but do not set bIsError.
            TVector<FCompilationError>  Warnings;
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
        CEdNodeGraph* GetActiveNodeGraph() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void SetupWorldForTool() override;

        void DrawViewport(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture) override;
        void DrawViewportOverlayElements(const FUpdateContext& UpdateContext, ImTextureRef ViewportTexture, ImVec2 ViewportSize) override;
        bool ShouldGenerateThumbnailOnSave() const override { return true; }
        void OnPostUndoRedo() override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawMaterialGraph();
        void DrawCompileErrorBanner();
        void DrawMaterialProperties();
        void DrawShaderStats();

        // Shared by the error and warning blocks, which differ only in palette.
        void DrawDiagnosticRows(const TVector<FCompilationError>& Diagnostics, bool bIsError);
        // Compiled-shader figures from the driver, unlike the source-derived estimates elsewhere in the panel.
        void DrawGPUStats(float LabelWidth, const ImVec4& HeaderColor, const ImVec4& LabelColor, const ImVec4& ValueColor);

        // bNotifyResult off lets a save-driven compile toast after the package save toast instead of under it.
        void Compile(bool bNotifyResult = true);

        // Toasts the outcome of the last compile: error, warnings, or clean.
        void NotifyCompileResult();
        void ApplyMaterialToPreview();

        // Lands finished pin and node previews and starts the ones the graph now wants.
        void UpdatePreviews();

        // While a pin is previewed, shows its value under the cursor in the viewport.
        void UpdatePixelProbe(ImVec2 ImageMin, ImVec2 ImageSize);

        // The transient spray the Particle domain previews through, pointed at PreviewMaterial.
        CParticleSystem* GetOrCreatePreviewParticleSystem(CMaterialInterface* PreviewMaterial);
        void FocusGraphNode(CEdGraphNode* Node);

        // Double-click handler for a Material Function call node: opens the referenced asset in its own tab.
        void OpenMaterialFunctionForNode(CEdGraphNode* Node);

        // Bound lazily to the selection, with edits written straight back to the node.
        void DrawCustomCodeEditor();

        void ConfigureCodeEditor();

        // Puts this node's pin names in scope, so they color and complete like the pixel pass's own locals.
        void RebuildCodeEditorLanguage(const CMaterialExpression_CustomSlang* Node);
        void OnSave() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    protected:
        
        void DrawHelpMenu() override;
        
    private:

        friend struct FMaterialEditorToolTestAccess;

        void SetDebugMesh(EDebugMesh Mesh, FStringView Path = "");
        
    private:
        
        ECS::FEntity                    MeshEntity;
        ECS::FEntity                    DirectionalLightEntity;
        // Always present; its system is null unless the asset is a Particle material, which extract skips.
        ECS::FEntity                    ParticleEntity;
        // Dark unless the asset is a LightFunction material, which it then projects onto the preview mesh.
        ECS::FEntity                    LightFunctionSpotEntity;

        // Transient spray the Particle domain previews through, built the first time one is opened.
        TStrongObjectPtr<CParticleSystem> PreviewParticleSystem;
        
        FString                         Tree;
        FString                         VertexTree;
        // True when the graph changed since the last successful Compile.
        NODISCARD bool NeedsCompile() const;

        FMaterialCompiler::FShaderStats ShaderStats;
        size_t                          ReplacementStart = 0;
        size_t                          ReplacementEnd = 0;
        CEdGraphNode*                   SelectedNode = nullptr;

        FSlangLanguage                  CodeEditorLanguage;
        FCodeEditor                     CodeEditor;
        // The node whose code the buffer holds, so changing selection reloads rather than writing one node's text into another.
        CEdGraphNode*                   CodeEditorBoundNode = nullptr;
        // Pin signature the highlight was last built from; pins are edited elsewhere while this tab stays bound.
        uint64                          CodeEditorPinSignature = 0;
        // Buffer version at the last write-back, so only a real edit dirties the package.
        uint64                          LastCodeEditorVersion = 0;

        // Open while a typing session in the code panel has unrecorded changes.
        bool                            bCodeEditOpen = false;
        FCompilationResultInfo          CompilationResult;
        // False until the first compile, so a save before one never toasts a result nothing produced.
        bool                            bHasCompiled = false;
        
        
        TUniquePtr<FPropertyTable>      EnvironmentEditor;
        TUniquePtr<FPropertyTable>      DirectionalEditor;
        
        TStrongObjectPtr<CMaterialNodeGraph> NodeGraph;

        // The pin the viewport shows in place of the material, while the graph's PreviewPinID names one.
        FPinPreviewMaterial             ViewportPreview;
        bool                            bShowingPinPreview = false;
        FMaterialNodePreviews           NodePreviews;
        FViewportPixelProbe             PixelProbe;
        bool                            bGLSLPreviewDirty = false;
        
        EDebugMesh                      DebugMesh;
    };
}
