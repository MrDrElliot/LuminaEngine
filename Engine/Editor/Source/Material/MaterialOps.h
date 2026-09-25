#pragma once

#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Containers/Vector.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"

namespace Lumina
{
    class CClass;
    class CMaterial;
    class CMaterialNodeGraph;
}

namespace Lumina::MaterialOps
{
    // Node types a material graph will accept, discovered from reflection rather than a list.
    NODISCARD EDITOR_API TVector<CClass*> GetPlaceableNodeTypes();

    NODISCARD EDITOR_API CClass* ResolveNodeType(FStringView TypeName);

    NODISCARD EDITOR_API CEdGraphNode* FindOutputNode(CMaterialNodeGraph* Graph);

    // The graph the material editor would open, created if the material has none yet.
    NODISCARD EDITOR_API CMaterialNodeGraph* FindOrCreateGraph(CMaterial* Material);
}
