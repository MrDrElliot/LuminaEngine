#include "RuntimePCH.h"

#include "GameLibrary.h"

#include "Core/Engine/Engine.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

namespace Lumina
{
    void CGameLibrary::QuitGame()
    {
        if (Global::GEngine != nullptr)
        {
            Global::GEngine->RequestExitGame();
        }
    }

    bool CGameLibrary::IsHeadless()
    {
        return Global::GIsHeadless;
    }
}
