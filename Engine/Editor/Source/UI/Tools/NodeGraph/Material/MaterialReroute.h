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
    // Material-typed reroute: hands the base reroute its CMaterialInput / CMaterialOutput pin classes
    // so type-aware compiler walks (GetTypedInputValue, etc.) keep working through it.
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
