#include "EditorPCH.h"
#include "MaterialNode_Grass.h"

namespace Lumina
{
    void CMaterialExpression_GrassOutput::BuildNode()
    {
        // No pins at all: the node carries data, not a value, so it stays out of the emit closure.
    }
}
