#pragma once

#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "MaterialGraphNode.generated.h"

namespace Lumina
{
    class FMaterialCompiler;
}

namespace Lumina
{
    // NotPlaceable: this is the family base, not a node anyone drops on a canvas. The specifier does not
    // inherit, so every concrete node below it stays discoverable.
    REFLECT(NotPlaceable)
    class CMaterialGraphNode : public CEdGraphNode
    {
        GENERATED_BODY()
    public:

        // Every material node, wherever it is declared, belongs to the material graph -- and to the
        // material FUNCTION graph, which derives from it. Declared once here so a node in a game or
        // plugin module needs nothing but this base to show up in the palette.
        CClass* GetSupportedGraphClass() const override;

        virtual void GenerateDefinition(FMaterialCompiler& Compiler) { UNREACHABLE(); }
        virtual void* GetNodeDefaultValue() { return nullptr; }
        virtual void SetNodeValue(void* Value) { }
        
        #if USING(WITH_EDITOR)
        void PostPropertyChange(FProperty* ChangedProperty) override;
        #endif
    };
    
}

