#include "RuntimePCH.h"
#include "GameplayTagsSettings.h"

#include "GameplayTags/GameplayTagRegistry.h"

namespace Lumina
{
    void CGameplayTagsSettings::PostInitSettings()
    {
        for (const FString& Tag : Tags)
        {
            FGameplayTagRegistry::Get().RequestTag(FStringView(Tag.c_str(), Tag.size()));
        }
    }
}
