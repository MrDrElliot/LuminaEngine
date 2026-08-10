#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Math.h"
#include "MaterialFunction.generated.h"

namespace Lumina
{
    // Material function pin value type. Mapped to/from the editor's EMaterialInputType by
    // ToMaterialInputType / ToMaterialValueType, which restate these ordinals -- APPEND ONLY, and add the
    // matching MaterialValueOrdinal entry (MaterialGraphTypes.h) when you do.
    //
    // A sampled texture is not a value type here: it is a float4, which Float4 already covers. TextureHandle
    // is the bindless index itself (a uint), so hand-written Slang inside a function can sample it directly.
    REFLECT()
    enum class EMaterialValueType : uint8
    {
        Float,
        Float2,
        Float3,
        Float4,
        TextureHandle,
    };

    // One declared function input; mirrored from the editor's FunctionInput nodes on save so a call
    // node can build pins without loading the function's editor graph.
    REFLECT()
    struct RUNTIME_API FMaterialFunctionInput
    {
        GENERATED_BODY()

        PROPERTY()
        FName Name;

        PROPERTY()
        EMaterialValueType Type = EMaterialValueType::Float;

        /** Used when a call node leaves this input pin unconnected. */
        PROPERTY()
        FVector4 DefaultValue = FVector4(0.0f);

        PROPERTY()
        FString Description;
    };

    // One declared output of a material function.
    REFLECT()
    struct RUNTIME_API FMaterialFunctionOutput
    {
        GENERATED_BODY()

        PROPERTY()
        FName Name;

        PROPERTY()
        EMaterialValueType Type = EMaterialValueType::Float;

        PROPERTY()
        FString Description;
    };

    // Reusable material subgraph, inlined into the host material's shader at compile time. Runtime only
    // needs the I/O signature + description; the editable node graph is a child object in this package.
    REFLECT()
    class RUNTIME_API CMaterialFunction : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        const TVector<FMaterialFunctionInput>&  GetInputs() const  { return Inputs; }
        const TVector<FMaterialFunctionOutput>& GetOutputs() const { return Outputs; }

        /** Human-readable summary shown in the call node tooltip / library. */
        PROPERTY(Editable, Category = "Material Function")
        FString Description;

        /** Derived from the function graph's FunctionInput nodes on save; do not hand-edit. */
        PROPERTY()
        TVector<FMaterialFunctionInput> Inputs;

        /** Derived from the function graph's FunctionOutput nodes on save; do not hand-edit. */
        PROPERTY()
        TVector<FMaterialFunctionOutput> Outputs;
    };
}
