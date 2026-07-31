#pragma once

#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "MaterialNodeGraph.generated.h"

namespace Lumina
{
    class CMaterial;
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

        void ValidateGraph() override;

        // Hold-and-click quick-place: 1..4 -> ConstantFloat..Float4, 5 Time, 6 WorldPos, 7 TexCoords,
        // 8 VertexNormal, 9 Multiply, 0 Add. Extend the dispatch table in HandleQuickPlace for more.
        void HandleQuickPlace(int Digit, ImVec2 CanvasPos) override;

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

        void SetMaterial(CMaterial* InMaterial);
        CMaterial* GetMaterial() const { return Material; }

    protected:

        // Registers the shared library of material expression nodes (math, inputs, textures, the
        // function-call node, reroute, ...). Shared by material and material-function graphs.
        void RegisterCommonMaterialNodes();

        // Creates always-present nodes for this graph kind: the material graph ensures one
        // CMaterialOutputNode; the function graph overrides to create nothing.
        virtual void EnsureRootNodes();

        // Hook for a graph kind to register its own extra node types after the common set. The
        // function graph registers FunctionInput / FunctionOutput here; the base adds nothing.
        virtual void RegisterGraphTypeNodes() {}

    private:

        TObjectPtr<CMaterial> Material;
    };
}
