#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "AudioNodeGraph.generated.h"

namespace Lumina
{
    class CAudioGraph;
    class CAudioGraphOutputNode;

    REFLECT()
    class CAudioNodeGraph : public CEdNodeGraph
    {
        GENERATED_BODY()

    public:

        void Initialize() override;
        void ValidateGraph() override;

        const FEdGraphSchema& GetSchema() const override;

        bool ShouldDrawInlinePinEditors() const override { return true; }

        /** The single output node, which everything the compiler emits is reachable backwards from. */
        bool IsGraphRootNode(CEdGraphNode* Node) const override;

        // Dropping a wave anywhere on the canvas spawns the Wave Player that plays it.
        void DrawCanvasDropTarget() override;

        void SetAudioGraph(CAudioGraph* InGraph) { AudioGraph = InGraph; }
        CAudioGraph* GetAudioGraph() const { return AudioGraph; }

        CAudioGraphOutputNode* FindOutputNode() const;

    private:

        void EnsureRootNodes();

        TObjectPtr<CAudioGraph> AudioGraph;
    };
}
