#include "RuntimePCH.h"

#include "Core/Engine/Engine.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "World/World.h"

namespace Lumina
{
    void CWorld::OpenLevel(const FString& Url)
    {
        if (Global::GEngine != nullptr && !Url.IsEmpty())
        {
            Global::GEngine->OpenLevel(FURL::Parse(FStringView(*Url, Url.Len())), this);
        }
    }
}
