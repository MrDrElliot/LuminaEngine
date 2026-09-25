#pragma once

#include "Containers/StringView.h"
#include "Containers/Vector.h"

namespace Lumina
{
    class CAnimationGraph;
    class CAnimationGraphNodeGraph;
    class CAnimStateTransition;
    class CClass;
    class CEdNodeGraph;
    class FAnimationGraphCompiler;
}

namespace Lumina::AnimGraphOps
{
    // The root blend tree the anim graph editor opens, created if the asset has none yet.
    NODISCARD EDITOR_API CAnimationGraphNodeGraph* FindOrCreateGraph(CAnimationGraph* Asset);

    // A blend tree and a state machine canvas accept different node types.
    NODISCARD EDITOR_API TVector<CClass*> GetPlaceableNodeTypes(CEdNodeGraph* Graph);

    NODISCARD EDITOR_API CClass* ResolveNodeType(CEdNodeGraph* Graph, FStringView TypeName);

    // Empty unless Graph is a state machine canvas.
    NODISCARD EDITOR_API TVector<CAnimStateTransition*> GetTransitions(CEdNodeGraph* Graph);

    // Leaves Asset's bytecode untouched when Compiler reports an error.
    EDITOR_API bool Compile(CAnimationGraph* Asset, CAnimationGraphNodeGraph* NodeGraph,
        FAnimationGraphCompiler& Compiler);
}
