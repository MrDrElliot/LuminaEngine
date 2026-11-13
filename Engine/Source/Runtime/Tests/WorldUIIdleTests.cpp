#include <gtest/gtest.h>

#include <limits>

#include "Core/Object/ObjectCore.h"
#include "GUID/GUID.h"
#include "UI/RmlUiBridge.h"
#include "World/World.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    FWorldUIContext& NewWorldUI(CWorld*& OutWorld)
    {
        OutWorld = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        FWorldTestAccess::CreateUIContext(*OutWorld);
        return *OutWorld->GetUIContext();
    }
}

TEST(WorldUIIdle, InputOnOneWorldLeavesTheOtherIdle)
{
    CWorld* Busy = nullptr;
    CWorld* Quiet = nullptr;
    FWorldUIContext& BusyUI = NewWorldUI(Busy);
    FWorldUIContext& QuietUI = NewWorldUI(Quiet);

    const FUIntVector2 Size(1920, 1080);
    for (FWorldUIContext* UI : { &BusyUI, &QuietUI })
    {
        EXPECT_TRUE(UI->NeedsUpdate(Size, 1.0f, 0.0));
        UI->NextUpdateSeconds = std::numeric_limits<double>::infinity();
    }
    const uint64 QuietSeen = QuietUI.SeenChangeGeneration;

    for (int32 Frame = 1; Frame <= 3; ++Frame)
    {
        {
            RmlUi::FLockedWorldContext ForwardedMouse(Busy);
        }
        RmlUi::GetContextForWorld(Busy);

        EXPECT_TRUE(BusyUI.NeedsUpdate(Size, 1.0f, Frame));
        EXPECT_FALSE(BusyUI.bIdleThisFrame);

        EXPECT_FALSE(QuietUI.NeedsUpdate(Size, 1.0f, Frame));
        EXPECT_TRUE(QuietUI.bIdleThisFrame);
        EXPECT_EQ(QuietUI.SeenChangeGeneration, QuietSeen);
    }

    RmlUi::DestroyWorldUI(Busy);
    RmlUi::DestroyWorldUI(Quiet);
}
