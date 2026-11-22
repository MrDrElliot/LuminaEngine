#pragma once
#include "Core/Object/ObjectMacros.h"
#include "UI/Tools/NodeGraph/EdNode_Reroute.h"
#include "MaterialReroute.generated.h"

namespace Lumina
{
    class CClass;
}

namespace Lumina
{
    // Supplies the material pin classes so type-aware compiler walks keep working through the reroute.
    REFLECT()
    class CMaterialReroute : public CEdNode_Reroute
    {
        GENERATED_BODY()
    public:

        // Not a CMaterialGraphNode (it derives from the generic reroute), so it names its graph itself.
        CClass* GetSupportedGraphClass() const override;

        CClass* GetInputPinClass() const override;
        CClass* GetOutputPinClass() const override;
    };
}
