#pragma once

#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Containers/Name.h"
#include "Containers/StringView.h"
#include "Containers/Vector.h"
#include "World/ECS/Entity.h"

namespace Lumina
{
    class CAnimationGraph;
    class CAnimationGraphNodeGraph;
    class CAnimStateTransition;
    class CClass;
    class CEdNodeGraph;
    class CStruct;
    class CWorld;
    class FAnimationGraphCompiler;
    struct FAnimGraphCrowdView;
}

namespace Lumina::AnimGraphOps
{
    // The root blend tree the anim graph editor opens, created if the asset has none yet.
    NODISCARD EDITOR_API CAnimationGraphNodeGraph* FindOrCreateGraph(CAnimationGraph* Asset);

    // Empty unless Graph is a state machine canvas.
    NODISCARD EDITOR_API TVector<CAnimStateTransition*> GetTransitions(CEdNodeGraph* Graph);

    // Leaves Asset's bytecode untouched when Compiler reports an error.
    EDITOR_API bool Compile(CAnimationGraph* Asset, CAnimationGraphNodeGraph* NodeGraph,
        FAnimationGraphCompiler& Compiler);

    // Points every parameter path under OldInput at NewInput, in every node and transition of every nested graph, and returns the graphs it changed.
    EDITOR_API TVector<CEdNodeGraph*> RenameInputPaths(CAnimationGraphNodeGraph* Root, FName OldInput, FName NewInput);

    // The component an input reads on Entity, or on its nearest ancestor that has one when Lookup climbs, else null.
    NODISCARD EDITOR_API void* FindInputComponent(CWorld& World, ECS::FEntity Entity, const CStruct& Struct, EAnimGraphInputLookup Lookup);

    // Which nodes of the asset just compiled by Compiler the crowd plan skips, keeping whether the view is shown.
    EDITOR_API void BuildCrowdView(const CAnimationGraph& Asset, const FAnimationGraphCompiler& Compiler, FAnimGraphCrowdView& Out);
}
