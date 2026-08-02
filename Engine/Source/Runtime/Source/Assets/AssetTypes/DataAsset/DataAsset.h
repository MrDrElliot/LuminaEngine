#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Reflection/PropertyBag/PropertyBag.h"
#include "DataAssetSchema.h"
#include "DataAsset.generated.h"

namespace Lumina
{
    // Data instance: a CDataAssetSchema ref + that schema's field values in an FPropertyBag. The schema owns
    // the structure; SyncToSchema rebuilds the value layout (preserving values by name) on edit/load.
    REFLECT()
    class RUNTIME_API CDataAsset : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        void PostLoad() override;
        void Serialize(FArchive& Ar) override;

        CDataAssetSchema* GetSchema() const { return Schema; }

        // Assigns the schema and immediately rebuilds the value layout to match it.
        void SetSchema(CDataAssetSchema* InSchema);

        // Rebuilds the value bag to the schema's field list (values kept by name, new fields from defaults).
        void SyncToSchema();

        // On schema delete: drops the dangling reference and clears the value bag.
        void OnSchemaDeleted();

        FPropertyBag& GetPropertyBag()             { return PropertyBag; }
        const FPropertyBag& GetPropertyBag() const { return PropertyBag; }

        // Serialized; set at creation. Not marked Editable, so it stays out of the value grid.
        PROPERTY()
        TObjectPtr<CDataAssetSchema> Schema;

    private:

        // The instance's values; layout mirrors Schema. Not a reflected PROPERTY.
        FPropertyBag PropertyBag;
    };
}
