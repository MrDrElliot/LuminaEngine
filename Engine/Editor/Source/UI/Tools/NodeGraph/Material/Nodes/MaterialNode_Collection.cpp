#include "MaterialNode_Collection.h"

#include "Assets/AssetTypes/Material/MaterialParameterCollection.h"
#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"

namespace Lumina
{
    void CMaterialExpression_CollectionParameter::DrawNodeBody()
    {
        const char* CollectionName = Collection.IsValid() ? Collection->GetName().c_str() : "<no collection>";
        ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.75f, 1.0f), "%s", CollectionName);

        if (ParameterName.IsNone())
        {
            ImGui::TextDisabled("<no parameter>");
        }
        else
        {
            ImGui::TextUnformatted(ParameterName.c_str());
        }
    }

    void CMaterialExpression_CollectionScalar::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float);
    }

    void CMaterialExpression_CollectionScalar::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        // The UI domain is a fullscreen brush with no scene root, so it has no collection table at all.
        if (Compiler.RejectInUI(this, "Collection Scalar"))
        {
            Compiler.DefineConstantFloat(FullName, 0.0f);
            return;
        }

        Compiler.DefineCollectionScalar(FullName, Collection.Get(), ParameterName, this);
    }

    void CMaterialExpression_CollectionVector::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float4);
        Output->SetComponentMask(EComponentMask::RGBA);
    }

    void CMaterialExpression_CollectionVector::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        if (Compiler.RejectInUI(this, "Collection Vector"))
        {
            float Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            Compiler.DefineConstantFloat4(FullName, Zero);
            return;
        }

        Compiler.DefineCollectionVector(FullName, Collection.Get(), ParameterName, this);
    }
}
