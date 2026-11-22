#pragma once

#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "MaterialGraphNode.generated.h"

namespace Lumina
{
    class FMaterialCompiler;
}

namespace Lumina
{
    // NotPlaceable does not inherit, so every concrete node below stays discoverable.
    REFLECT(NotPlaceable, Abstract)
    class CMaterialGraphNode : public CEdGraphNode
    {
        GENERATED_BODY()
    public:
        
        CClass* GetSupportedGraphClass() const override;

        virtual void GenerateDefinition(FMaterialCompiler& Compiler) = 0;
        virtual void* GetNodeDefaultValue() { return nullptr; }
        virtual void SetNodeValue(void* Value) { }

        #if USING(WITH_EDITOR)
        void PostPropertyChange(FProperty* ChangedProperty) override;
        #endif
    };
    
}

