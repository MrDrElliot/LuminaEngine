#pragma once

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Math.h"
#include "MaterialFunction.generated.h"

namespace Lumina
{
    // Pin value type whose ordinals are restated by ToMaterialInputType, so append only and add the matching MaterialValueOrdinal entry.
    REFLECT()
    enum class EMaterialValueType : uint8
    {
        Float,
        Float2,
        Float3,
        Float4,
        TextureHandle,
    };

    // One declared function input, mirrored from the editor's FunctionInput nodes on save.
    REFLECT()
    struct RUNTIME_API SMaterialFunctionInput
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FName Name;

        PROPERTY(Hidden)
        EMaterialValueType Type = EMaterialValueType::Float;

        /** Used when a call node leaves this input pin unconnected. */
        PROPERTY(Hidden)
        FVector4 DefaultValue = FVector4(0.0f);

        PROPERTY(Hidden)
        FString Description;
    };

    // One declared output of a material function.
    REFLECT()
    struct RUNTIME_API SMaterialFunctionOutput
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FName Name;

        PROPERTY(Hidden)
        EMaterialValueType Type = EMaterialValueType::Float;

        PROPERTY(Hidden)
        FString Description;
    };

    // Reusable material subgraph inlined into the host shader, with the editable graph kept as a child object.
    REFLECT()
    class RUNTIME_API CMaterialFunction : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        const TVector<SMaterialFunctionInput>&  GetInputs() const  { return Inputs; }
        const TVector<SMaterialFunctionOutput>& GetOutputs() const { return Outputs; }

        /** Human-readable summary shown in the call node tooltip / library. */
        PROPERTY(Editable, Category = "Material Function")
        FString Description;

        /** Derived from the function graph's FunctionInput nodes on save; do not hand-edit. */
        PROPERTY(Hidden)
        TVector<SMaterialFunctionInput> Inputs;

        /** Derived from the function graph's FunctionOutput nodes on save; do not hand-edit. */
        PROPERTY(Hidden)
        TVector<SMaterialFunctionOutput> Outputs;
    };
}
