#pragma once

#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "MaterialGraphNode.generated.h"

namespace Lumina
{
    class FMaterialCompiler;
}

namespace Lumina
{
    REFLECT()
    class CMaterialGraphNode : public CEdGraphNode
    {
        GENERATED_BODY()
    public:
        
        virtual void GenerateDefinition(FMaterialCompiler& Compiler) { UNREACHABLE(); }
        virtual void* GetNodeDefaultValue() { return nullptr; }
        virtual void SetNodeValue(void* Value) { }
        
        #if USING(WITH_EDITOR)
        void PostPropertyChange(FProperty* ChangedProperty) override;
        #endif
    };
    
}

