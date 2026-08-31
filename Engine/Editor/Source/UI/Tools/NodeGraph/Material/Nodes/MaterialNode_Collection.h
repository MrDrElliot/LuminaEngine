#pragma once
#include "MaterialNodeExpression.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "MaterialNode_Collection.generated.h"

namespace Lumina
{
    class CMaterialParameterCollection;

    // Shared base for the two collection reads; not a node in its own right.
    REFLECT(NotPlaceable)
    class CMaterialExpression_CollectionParameter : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        FFixedString GetNodeCategory() const override { return "Parameters"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(40, 130, 120, 255); }

        void DrawNodeBody() override;

        FName* GetParameterName() override { return &ParameterName; }

        /** The collection to read. A material may bind at most MAX_MATERIAL_COLLECTIONS of them. */
        PROPERTY(Editable, Category = "Collection")
        TObjectPtr<CMaterialParameterCollection> Collection;

        /** Name the collection declares. Its position in the collection is what the shader compiles to. */
        PROPERTY(Editable, Category = "Collection")
        FName ParameterName;
    };

    REFLECT()
    class CMaterialExpression_CollectionScalar : public CMaterialExpression_CollectionParameter
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;

        FStringView GetNodeDisplayName() const override { return "Collection Scalar"; }
        FStringView GetNodeTooltip() const override
        {
            return "Reads a scalar out of a material parameter collection. The value is shared by every "
                   "material that binds the collection, so setting it once reaches every surface.";
        }

        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_CollectionVector : public CMaterialExpression_CollectionParameter
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;

        FStringView GetNodeDisplayName() const override { return "Collection Vector"; }
        FStringView GetNodeTooltip() const override
        {
            return "Reads a float4 out of a material parameter collection. The value is shared by every "
                   "material that binds the collection, so setting it once reaches every surface.";
        }

        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };
}
