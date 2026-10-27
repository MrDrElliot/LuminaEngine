#pragma once

#include "MaterialGraphTypes.h"
#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "MaterialInput.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialInput : public CEdNodeGraphPin
    {
        GENERATED_BODY()
        
    public:
        
        float DrawPin() override;
        FString GetPinTypeName() const override;

        // Scalar inputs only, since every node lays its scalar defaults out as floats indexed by pin.
        bool HasInlineEditor() const override;
        EGraphPinShape GetPinShape() const override;

        // Width decides the shape, so float through float4 read apart without color, and a set mask decides the width.
        static EGraphPinShape ShapeFor(EMaterialInputType Type, EComponentMask Mask);

        void SetInputType(EMaterialInputType InType) { InputType = InType; }
        EMaterialInputType GetInputType() const { return InputType; }

        EComponentMask GetComponentMask() const { return Mask; }
        void SetComponentMask(EComponentMask InMask) { Mask = InMask; }
        void SetIndex(uint32 InIndex) { Index = InIndex; }
        uint32 GetIndex() const { return Index; }
        
    private:

        /** Index on the node's pin array, useful for offsetting for properties */
        uint32              Index = 0;
        
        EMaterialInputType  InputType = EMaterialInputType::Float;

        EComponentMask      Mask = EComponentMask::RGBA;
    };

}
