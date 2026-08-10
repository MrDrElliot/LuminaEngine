#include "MaterialReroute.h"

#include "MaterialInput.h"
#include "MaterialOutput.h"
#include "Core/Object/Class.h"
#include "MaterialNodeGraph.h"

namespace Lumina
{
    CClass* CMaterialReroute::GetSupportedGraphClass() const
    {
        return CMaterialNodeGraph::StaticClass();
    }

    CClass* CMaterialReroute::GetInputPinClass() const
    {
        return CMaterialInput::StaticClass();
    }

    CClass* CMaterialReroute::GetOutputPinClass() const
    {
        return CMaterialOutput::StaticClass();
    }
}
