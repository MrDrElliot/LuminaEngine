#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Reflection/PropertyBag/PropertyBag.h"
#include "DataAssetSchema.generated.h"

namespace Lumina
{
    // Shape + defaults for a family of data assets, held in an FPropertyBag (layout = field list,
    // values = defaults). Editing it re-syncs every loaded instance via PropagateToInstances.
    REFLECT()
    class RUNTIME_API CDataAssetSchema : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        void Serialize(FArchive& Ar) override;

        // Deleting orphans instances; clears their reference + values so nothing dangles.
        void OnDestroy() override;

        FPropertyBag& GetSchemaBag()             { return SchemaBag; }
        const FPropertyBag& GetSchemaBag() const { return SchemaBag; }

        // Re-syncs every loaded CDataAsset referencing this schema to the current layout.
        void PropagateToInstances();

    private:

        // Field list + per-field defaults. Not a reflected PROPERTY; serialized manually.
        FPropertyBag SchemaBag;
    };
}
