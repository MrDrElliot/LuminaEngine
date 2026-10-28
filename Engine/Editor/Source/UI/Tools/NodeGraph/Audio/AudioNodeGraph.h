#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "AudioNodeGraph.generated.h"

namespace Lumina
{
    class CAudioGraph;
    class CAudioGraphInputNode;
    class CAudioNode_WavePlayer;
    class CAudioGraphOutputNode;

    REFLECT()
    class EDITOR_API CAudioNodeGraph : public CEdNodeGraph
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
        CAudioGraph* GetAudioGraph() const { return AudioGraph.Get(); }

        CAudioGraphOutputNode* FindOutputNode() const;

        // The trigger input every graph raises on its first block, which is what starts a Wave Player.
        CAudioGraphInputNode* FindOnPlayNode() const;

        // A dropped wave plays as soon as the graph does, through any Output pins still free.
        void WireDroppedWavePlayer(CAudioNode_WavePlayer* Player);

    private:

        void EnsureRootNodes();

        TStrongObjectPtr<CAudioGraph> AudioGraph;
    };
}
