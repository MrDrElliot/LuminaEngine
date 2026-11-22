#pragma once

#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "MaterialNodeGraph.generated.h"

namespace Lumina
{
    class CMaterial;
    class CMaterialOutput;
    class FMaterialCompiler;
}

namespace Lumina
{
    REFLECT()
    class CMaterialNodeGraph : public CEdNodeGraph
    {
        GENERATED_BODY()
        
    public:
        
        void Initialize() override;
        void Shutdown() override;
        
        void CompileGraph(FMaterialCompiler& Compiler);

        // Stamps the compiler's findings onto the nodes that raised them, driving their in-graph status.
        void ApplyDiagnosticsToNodes(const FMaterialCompiler& Compiler);

        // Adds the declared and actually-received types, which is what a mismatch error is about.
        void DrawPinTooltip(CEdNodeGraphPin* Pin) override;

        bool ShouldDrawInlinePinEditors() const override { return true; }

        // Vector wires draw wider, so a float4 reads heavier than a float at a glance.
        void GetLinkStyle(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, ImU32& OutColor, float& OutThickness) const override;

        // A wire into a strictly typed input of another width, which the compiler pads, broadcasts or swizzles.
        bool GetLinkBadge(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, FString& OutText, ImU32& OutColor) override;

        // Wires downstream of anything whose compiled code reads GetTime().
        bool WantsLinkFlow(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin) override;

        bool GetNodeHeat(const CEdGraphNode* Node, float& OutHeat, FString& OutLabel) override;
        bool GetNodeGeneratedCode(const CEdGraphNode* Node, FString& OutCode) override;

        // Shades nodes by the shader work each one added in the last compile.
        bool bShowCostHeat = false;

        // The output pin the viewport shows in place of the material, or zero.
        uint32 PreviewPinID = 0;

        // Hands back a node's rendered preview, or what is happening instead, from the editor that renders them.
        TFunction<bool(int64, ImTextureRef&, const char*&)> NodePreviewSource;

        void DrawPinContextMenu(CEdNodeGraphPin* Pin) override;
        void DrawNodeMenuItems(CEdGraphNode* Node) override;
        bool HasNodePreview(const CEdGraphNode* Node) override;
        void DrawNodePreviewContent(CEdGraphNode* Node) override;
        bool IsNodePreviewed(const CEdGraphNode* Node) override;

        // Moves the selection into a new material function beside this material, wired in through one call node.
        bool CollapseSelectionToFunction(FString& OutPath, FString& OutError);

        void ValidateGraph() override;

        // Hold-and-click quick-place, with the digit dispatch table in HandleQuickPlace.
        void HandleQuickPlace(int Digit, ImVec2 CanvasPos) override;

        // Letter variant, declared beside the int overload so overriding one does not hide the other.
        void HandleQuickPlace(char Key, ImVec2 CanvasPos) override;

        // Spawns a TextureSample or CurveSample for an asset dropped on the canvas.
        void DrawCanvasDropTarget() override;

        // SetNodeValue casts without checking, so Asset must be the type NodeClass expects. Returns null if either is null.
        CEdGraphNode* SpawnAssetNode(CClass* NodeClass, CObject* Asset, ImVec2 ScreenPos);

        // Material reroutes carry typed pins so the existing compiler casts keep working.
        CClass* GetRerouteNodeClass() const override;

        // The schema keeps TextureHandle pins out of the float graph.
        const FEdGraphSchema& GetSchema() const override;

        // The single CMaterialOutputNode, from which everything the compiler emits is reachable.
        bool IsGraphRootNode(CEdGraphNode* Node) const override;

        void SetMaterial(CMaterial* InMaterial);
        CMaterial* GetMaterial() const { return Material.Get(); }

    protected:

        // Shared tail of both quick-place overloads: spawn at the cursor and select.
        void QuickPlaceNode(CClass* NodeClass, ImVec2 CanvasPos);

        // The material graph ensures one CMaterialOutputNode while the function graph creates nothing.
        virtual void EnsureRootNodes();

    private:

        // What one node emitted in the last compile.
        struct FNodeInsight
        {
            FString PixelCode;
            FString VertexCode;
            uint32  Cost       = 0;
            bool    bReadsTime = false;
        };

        void RefreshTimeVaryingNodes();

        // Emits what feeds Pin and nothing else, for a compile that previews one output.
        void CompilePreview(FMaterialCompiler& Compiler, CMaterialOutput* Pin);

        TStrongObjectPtr<CMaterial>     Material;
        THashMap<int64, FNodeInsight>   NodeInsights;
        uint32                          MaxNodeCost = 0;
        uint32                          InsightGeneration = 0;
        THashSet<const CEdGraphNode*>   TimeVaryingNodes;
        uint64                          TimeVaryingKey = ~0ull;
    };
}
