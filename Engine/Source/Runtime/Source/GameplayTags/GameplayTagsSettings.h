#pragma once

#include "Config/DeveloperSettings.h"
#include "Containers/String.h"
#include "Containers/Array.h"
#include "GameplayTagsSettings.generated.h"

namespace Lumina
{
    // Project-wide authored gameplay tags. Registered into FGameplayTagRegistry at load, so the editor tag
    // picker lists them (plus any tag requested at runtime). Persists to /Config/GameplayTags.json; the
    // picker's "+" button appends here.
    REFLECT(MinimalAPI, ConfigFile = "/Config/GameplayTags.json", DisplayName = "Gameplay Tags", Category = "Project")
    class CGameplayTagsSettings : public CDeveloperSettings
    {
        GENERATED_BODY()

    public:

        // Dotted tag names ("Ability.Fire.Fireball"). Ancestors are interned automatically.
        PROPERTY(Editable, Category = "Tags")
        TVector<FString> Tags;

        void PostInitSettings() override;
    };
}
