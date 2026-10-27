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

        // Hold-and-click quick-place: 1..4 -> ConstantFloat..Float4, 5 Time, 6 WorldPos, 7 TexCoords,
        // 8 VertexNormal, 9 Multiply, 0 Add. Extend the dispatch table in HandleQuickPlace for more.
        void HandleQuickPlace(int Digit, ImVec2 CanvasPos) override;

        // Letter variant of the same gesture: T -> TextureSample, C -> CurveSample. Declared alongside
        // the int overload so overriding one does not hide the other.
        void HandleQuickPlace(char Key, ImVec2 CanvasPos) override;

        // Accepts assets dropped anywhere on the canvas, spawning the node that samples them: textures
        // become a TextureSample, curves a CurveSample.
        void DrawCanvasDropTarget() override;

        // Adds NodeClass already pointed at Asset, positioned at ScreenPos (converted to canvas space on
        // the next draw). Asset is handed over through SetNodeValue, which casts without checking, so it
        // must be the type NodeClass expects. Returns null if either argument is null. Public so a tool
        // can reuse it for non-drop entry points.
        CEdGraphNode* SpawnAssetNode(CClass* NodeClass, CObject* Asset, ImVec2 ScreenPos);

        // Reroute class to spawn (enables the double-click-on-wire UX); material reroutes carry
        // typed CMaterialInput / CMaterialOutput pins so the existing compiler casts keep working.
        CClass* GetRerouteNodeClass() const override;

        // Float widths stay freely interconnectable; the schema exists to keep TextureHandle pins
        // (raw bindless indices) out of the float graph. See FMaterialGraphSchema.
        const FEdGraphSchema& GetSchema() const override;

        // The single CMaterialOutputNode. Everything the compiler emits is reachable backwards from it,
        // which is what makes the dead-node fade and Tidy meaningful here.
        bool IsGraphRootNode(CEdGraphNode* Node) const override;

        void SetMaterial(CMaterial* InMaterial);
        CMaterial* GetMaterial() const { return Material.Get(); }

    protected:

        // Shared tail of both quick-place overloads: spawn at the cursor and select.
        void QuickPlaceNode(CClass* NodeClass, ImVec2 CanvasPos);

        // Creates always-present nodes for this graph kind: the material graph ensures one
        // CMaterialOutputNode; the function graph overrides to create nothing.
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
