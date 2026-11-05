#pragma once

#include "Config/DeveloperSettings.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "HierarchicalTagsSettings.generated.h"

namespace Lumina
{
    // Project-wide authored hierarchical tags. Registered into FHierarchicalTagRegistry at load, so the editor tag
    // picker lists them (plus any tag requested at runtime). Persists to /Config/HierarchicalTags.json; the
    // picker's "+" button appends here.
    REFLECT(MinimalAPI, ConfigFile = "/Config/HierarchicalTags.json", DisplayName = "Hierarchical Tags", Category = "Project")
    class CHierarchicalTagsSettings : public CDeveloperSettings
    {
        GENERATED_BODY()

    public:

        // Dotted tag names ("Ability.Fire.Fireball"). Ancestors are interned automatically.
        PROPERTY(Editable, Category = "Tags")
        TVector<FString> Tags;

        void PostInitSettings() override;
    };
}
