#pragma once

#include "GraphEditor.h"

namespace Lumina
{
    // Lays a node out as a titled header over a column of inputs and a column of outputs.
    class FGraphNodeBuilder
    {
    public:

        explicit FGraphNodeBuilder(FGraphEditor& InEditor) : Editor(InEditor) {}

        void Begin(int64 InNodeID, ImVec2& InOutPosition);
        void End(bool bWantsTitlebar);

        void Header(ImU32 Color);
        void EndHeader();

        void Input(uint32 PinID, ImU32 Color);
        void EndInput();

        void Middle();

        void Output(uint32 PinID, ImU32 Color);
        void EndOutput();

        // Rows submitted after this span the node under its pins.
        void Footer();

        // A chevron bar along the bottom edge, true on the frame it is clicked.
        bool ExpandToggle(bool bExpanded);

        // A shape in a 24 unit square, filled once the pin has a wire.
        static void PinIcon(const FGraphPinIcon& Icon);

    private:

        enum class EStage : uint8
        {
            Invalid,
            Begin,
            Header,
            Content,
            Input,
            Output,
            Middle,
            Footer,
            End,
        };

        void SetStage(EStage NewStage);
        void DrawExpandBar(ImDrawList* DrawList, const ImVec2& NodeMax) const;

        FGraphEditor& Editor;
        int64         NodeID = 0;
        ImVec2        NodePosition;
        EStage        Stage = EStage::Invalid;
        ImU32         HeaderColor = 0;
        bool          bHasHeader = false;
        bool          bHasExpandBar = false;
        bool          bExpandBarHovered = false;
        bool          bExpanded = false;
        float         ExpandBarTop = 0.0f;
    };
}
