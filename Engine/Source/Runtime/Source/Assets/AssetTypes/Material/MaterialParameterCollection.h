#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "Renderer/MaterialTypes.h"
#include "MaterialParameterCollection.generated.h"

namespace Lumina
{
    REFLECT()
    struct RUNTIME_API FCollectionScalarParameter
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Parameter")
        FName ParameterName;

        PROPERTY(Editable, Category = "Parameter")
        float DefaultValue = 0.0f;
    };

    REFLECT()
    struct RUNTIME_API FCollectionVectorParameter
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Parameter")
        FName ParameterName;

        PROPERTY(Editable, Color, Category = "Parameter")
        FVector4 DefaultValue = FVector4(0.0f);
    };

    /** Scalars and vectors shared by every material that binds this, so one write reaches every surface. */
    REFLECT()
    class RUNTIME_API CMaterialParameterCollection : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        void PostLoad() override;
        void OnDestroy() override;
        void PostPropertyChange(FProperty* ChangedProperty) override;

        /** Sets one live value and uploads its bytes alone. False when no parameter carries that name. */
        FUNCTION()
        bool SetScalarValue(const FName& Name, float Value);

        FUNCTION()
        bool SetVectorValue(const FName& Name, FVector4 Value);

        FUNCTION()
        float GetScalarValue(const FName& Name, float Default = 0.0f) const;

        FUNCTION()
        FVector4 GetVectorValue(const FName& Name, FVector4 Default = FVector4(0.0f)) const;

        FUNCTION()
        bool HasScalarParameter(const FName& Name) const { return FindScalarIndex(Name) != INDEX_NONE; }

        FUNCTION()
        bool HasVectorParameter(const FName& Name) const { return FindVectorIndex(Name) != INDEX_NONE; }

        /** Slot in the GPU collection table, or INDEX_NONE when the table had none left. */
        NODISCARD int32 GetCollectionIndex() const { return CollectionIndex; }

        NODISCARD int32 FindScalarIndex(const FName& Name) const;
        NODISCARD int32 FindVectorIndex(const FName& Name) const;

        /** Declared scalars. A parameter's position here is the index the graph compiles into a shader. */
        PROPERTY(Editable, Category = "Collection")
        TVector<FCollectionScalarParameter> ScalarParameters;

        PROPERTY(Editable, Category = "Collection")
        TVector<FCollectionVectorParameter> VectorParameters;

    private:

        /** Replays every declared default into the block and pushes the whole thing. */
        void RebuildUniforms();

        /** Deliberately not serialized, since a slot belongs to a session rather than to the asset. */
        int32                       CollectionIndex = INDEX_NONE;

        /** The live values, which diverge from the declared defaults as soon as anything sets one. */
        FMaterialCollectionUniforms Uniforms = {};
    };
}
