#include <gtest/gtest.h>

#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/ObjectCore.h"
#include "GUID/GUID.h"
#include "UI/RmlUiBridge.h"
#include "World/World.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    constexpr const char* kTestDocument = "<rml><body><div id=\"target\">Click</div></body></rml>";

    // Counts live copies of a bound lambda, so the test sees when the listener that owns it is freed.
    struct FBindingCanary
    {
        int32* Live;
        explicit FBindingCanary(int32* InLive) : Live(InLive) { ++*Live; }
        FBindingCanary(const FBindingCanary& Other) : Live(Other.Live) { ++*Live; }
        ~FBindingCanary() { --*Live; }
    };
}

// A "handle once" pattern that unsubscribes its own listener from inside that listener's handler.
TEST(RmlUiEventListenerSelfRemoval, RemovingDuringDispatchDefersTheDelete)
{
    ASSERT_TRUE(RmlUi::Initialize(false));

    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    FWorldTestAccess::CreateUIContext(*World);

    void* Document = RmlUi::LoadScreenDocumentFromMemory(World, kTestDocument, "test://self_removal.rml");
    ASSERT_NE(Document, nullptr);
    RmlUi::ShowDocument(Document, false, false);

    void* Target = RmlUi::DocumentGetElementById(Document, "target");
    ASSERT_NE(Target, nullptr);

    void* Listener = RmlUi::AddElementEventListener(World, Target, "click");
    ASSERT_NE(Listener, nullptr);

    auto* Delegate = static_cast<TScriptDelegate<RmlUi::FUIEventData>*>(RmlUi::GetElementEventListenerDelegate(Listener));
    ASSERT_NE(Delegate, nullptr);

    int32 CallCount = 0;
    int32 LiveBindings = 0;
    Delegate->AddLambda([&, Canary = FBindingCanary(&LiveBindings)](const RmlUi::FUIEventData&)
    {
        ++CallCount;
        RmlUi::RemoveElementEventListener(World, Listener);
    });
    ASSERT_EQ(LiveBindings, 1);

    RmlUi::ElementClick(Target);
    EXPECT_EQ(CallCount, 1);

    // Still bound means the listener outlived the dispatch that removed it, instead of being freed under its own ProcessEvent.
    EXPECT_EQ(LiveBindings, 1);

    RmlUi::TickWorldUI(World);
    EXPECT_EQ(LiveBindings, 0);

    RmlUi::UnloadScreenDocument(World, Document);
    RmlUi::DestroyWorldUI(World);
    RmlUi::Shutdown();
}
