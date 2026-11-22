#pragma once

#include "MaterialGraphTypes.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "MaterialOutput.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialOutput : public CEdNodeGraphPin
    {
        GENERATED_BODY()
    public:
        
        float DrawPin() override;
        FString GetPinTypeName() const override;

        // A color set on the pin wins, otherwise the type picks one, so wires read by what they carry.
        uint32 GetPinColor() const override;
        EGraphPinShape GetPinShape() const override;
        EComponentMask GetComponentMask() const { return Mask; }
        void SetComponentMask(EComponentMask InMask) { Mask = InMask; }
        
        void SetInputType(EMaterialInputType Type) { InputType = Type; }

        // Compile-only override set by function-call nodes, where empty means the node's FullName, and rebound each compile.
        FString             ResolvedVar;

        EMaterialInputType  InputType = EMaterialInputType::Float;
        EComponentMask      Mask = EComponentMask::None;

    };
}
