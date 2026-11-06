#include "RuntimePCH.h"
#include "ScopedInputMode.h"

#include "World/Subsystems/InputSubsystem.h"
#include "World/World.h"

namespace Lumina
{
    FScopedInputMode::FScopedInputMode(CWorld* InWorld, EInputMode InputMode, EMouseMode MouseMode)
        : World(InWorld)
    {
        if (InWorld != nullptr)
        {
            Id = InWorld->GetInput()->AcquireInputMode(InputMode, MouseMode);
        }
    }

    FScopedInputMode::FScopedInputMode(FScopedInputMode&& Other) noexcept
        : World(Move(Other.World))
        , Id(Other.Id)
    {
        Other.Id = 0;
    }

    FScopedInputMode& FScopedInputMode::operator=(FScopedInputMode&& Other) noexcept
    {
        if (this != &Other)
        {
            Reset();
            World = Move(Other.World);
            Id = Other.Id;
            Other.Id = 0;
        }
        return *this;
    }

    void FScopedInputMode::Reset()
    {
        if (Id != 0)
        {
            if (CWorld* Held = World.Get())
            {
                Held->GetInput()->ReleaseInputMode(Id);
            }
            Id = 0;
        }
        World = nullptr;
    }
}
