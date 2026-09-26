#include <gtest/gtest.h>

#include "Containers/String.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/DelegateProperty.h"
#include "Core/Delegates/CoreDelegates.h"
#include "ScriptDelegateTestTypes.h"

using namespace Lumina;

namespace
{
    const FDelegateProperty* FindDelegate(const char* Name)
    {
        FProperty* Property = SDelegateTestHost::StaticStruct()->GetProperty(FName(Name));
        return static_cast<const FDelegateProperty*>(Property);
    }
}

TEST(ScriptDelegateReflection, EveryArityIsReflectedWithTheRightArgumentCount)
{
    ASSERT_NE(SDelegateTestHost::StaticStruct(), nullptr);

    EXPECT_EQ(FindDelegate("OnNothing")->GetNumArgs(), 0u);
    EXPECT_EQ(FindDelegate("OnOne")->GetNumArgs(), 1u);
    EXPECT_EQ(FindDelegate("OnTwo")->GetNumArgs(), 2u);
    EXPECT_EQ(FindDelegate("OnThree")->GetNumArgs(), 3u);
}

TEST(ScriptDelegateReflection, ArgumentsAreInDeclarationOrderWithTheirOwnKinds)
{
    const FDelegateProperty* Three = FindDelegate("OnThree");
    ASSERT_EQ(Three->GetNumArgs(), 3u);

    const TSpan<FProperty* const> Args = Three->GetArgs();

    EXPECT_EQ(Args[0]->TypeFlags, EPropertyTypeFlags::Struct);
    EXPECT_EQ(Args[1]->TypeFlags, EPropertyTypeFlags::Float);

    // The whole point of the rewrite; a non-blittable argument reflects as a real string property.
    EXPECT_EQ(Args[2]->TypeFlags, EPropertyTypeFlags::String);
}

// The managed side reads each argument at its property offset, so these must match the pack exactly.
TEST(ScriptDelegateReflection, ArgumentOffsetsMatchTheBroadcastPack)
{
    using FPack = TArgPack<SDelegateTestPayload, float, FString>;

    const FDelegateProperty* Three = FindDelegate("OnThree");
    const TSpan<FProperty* const> Args = Three->GetArgs();

    EXPECT_EQ((size_t)Args[0]->Offset, FPack::Off0());
    EXPECT_EQ((size_t)Args[1]->Offset, FPack::Off1());
    EXPECT_EQ((size_t)Args[2]->Offset, FPack::Off2());

    // A single-argument pack stays at offset zero, which is what keeps the old accessors valid.
    EXPECT_EQ((size_t)FindDelegate("OnOne")->GetArgs()[0]->Offset, 0u);
}

// A broadcast has to land values where the reflected offsets say they are, or the managed read is garbage.
TEST(ScriptDelegateReflection, BroadcastWritesArgumentsAtTheReflectedOffsets)
{
    SDelegateTestHost Host;

    SDelegateTestPayload SeenPayload{};
    float                SeenFloat = 0.0f;
    FString              SeenString;

    Host.OnThree.AddLambda([&](const SDelegateTestPayload& A, const float& B, const FString& C)
    {
        SeenPayload = A;
        SeenFloat = B;
        SeenString = C;
    });

    Host.OnThree.Broadcast(SDelegateTestPayload{ 2.0f, 5 }, 0.25f, FString("hello"));

    EXPECT_FLOAT_EQ(SeenPayload.X, 2.0f);
    EXPECT_EQ(SeenPayload.Count, 5);
    EXPECT_FLOAT_EQ(SeenFloat, 0.25f);
    EXPECT_STREQ(SeenString.c_str(), "hello");
}

TEST(ScriptDelegateReflection, DelegatePropertySizeIsTheDelegateNotThePack)
{
    // The slot in the owning struct is always one delegate, whatever the arguments are.
    EXPECT_EQ(FindDelegate("OnThree")->GetElementSize(), (uint32)sizeof(FScriptDelegate));
    EXPECT_EQ(FindDelegate("OnNothing")->GetElementSize(), (uint32)sizeof(FScriptDelegate));
}

// A named signature must reflect exactly as the spelled-out form does, alias or typedef alike.
TEST(ScriptDelegateReflection, AliasAndTypedefSignaturesReflectLikeTheSpelledForm)
{
    const FDelegateProperty* Aliased = FindDelegate("OnAliasedTwo");
    const FDelegateProperty* Direct  = FindDelegate("OnTwo");
    ASSERT_EQ(Aliased->GetNumArgs(), 2u);
    ASSERT_EQ(Direct->GetNumArgs(), 2u);

    EXPECT_EQ(Aliased->GetArgs()[0]->TypeFlags, Direct->GetArgs()[0]->TypeFlags);
    EXPECT_EQ(Aliased->GetArgs()[1]->TypeFlags, Direct->GetArgs()[1]->TypeFlags);
    EXPECT_EQ(Aliased->GetArgs()[0]->Offset, Direct->GetArgs()[0]->Offset);
    EXPECT_EQ(Aliased->GetArgs()[1]->Offset, Direct->GetArgs()[1]->Offset);

    const FDelegateProperty* Typedefed = FindDelegate("OnTypedefThree");
    const FDelegateProperty* Spelled   = FindDelegate("OnThree");
    ASSERT_EQ(Typedefed->GetNumArgs(), 3u);

    for (size_t Index = 0; Index < 3; ++Index)
    {
        EXPECT_EQ(Typedefed->GetArgs()[Index]->TypeFlags, Spelled->GetArgs()[Index]->TypeFlags);
        EXPECT_EQ(Typedefed->GetArgs()[Index]->Offset, Spelled->GetArgs()[Index]->Offset);
    }

    // The string argument still arrives as a string through the alias, not as an opaque struct.
    EXPECT_EQ(Typedefed->GetArgs()[2]->TypeFlags, EPropertyTypeFlags::String);
}

TEST(ScriptDelegateReflection, AnAliasedDelegateBroadcastsThroughItsAliasType)
{
    SDelegateTestHost Host;

    SDelegateTestPayload Seen{};
    float SeenFloat = 0.0f;

    Host.OnAliasedTwo.AddLambda([&](const SDelegateTestPayload& A, const float& B)
    {
        Seen = A;
        SeenFloat = B;
    });

    Host.OnAliasedTwo.Broadcast(SDelegateTestPayload{ 7.0f, 3 }, 1.5f);

    EXPECT_FLOAT_EQ(Seen.X, 7.0f);
    EXPECT_EQ(Seen.Count, 3);
    EXPECT_FLOAT_EQ(SeenFloat, 1.5f);
}

// The managed wrapper reads each event at its reflected offset over the one live singleton.
TEST(ScriptDelegateReflection, CoreDelegateSlotsMatchTheSingletonLayout)
{
    CStruct* Events = SCoreDelegates::StaticStruct();
    ASSERT_NE(Events, nullptr);

    SCoreDelegates& Live = FCoreDelegates::Get();
    const uint8* Base = reinterpret_cast<const uint8*>(&Live);

    struct FCase { const char* Name; const FScriptDelegateBase* Member; };
    const FCase Cases[] =
    {
        { "PostWorldUnload",     &Live.PostWorldUnload },
        { "OnPreEngineShutdown", &Live.OnPreEngineShutdown },
        { "OnGameQuitRequested", &Live.OnGameQuitRequested },
        { "OnInputPumped",       &Live.OnInputPumped },
    };

    for (const FCase& Case : Cases)
    {
        FProperty* Slot = Events->GetProperty(FName(Case.Name));
        ASSERT_NE(Slot, nullptr) << Case.Name;
        EXPECT_EQ(Base + Slot->Offset, reinterpret_cast<const uint8*>(Case.Member)) << Case.Name;
        EXPECT_EQ(static_cast<const FDelegateProperty*>(Slot)->GetNumArgs(), 0u) << Case.Name;
    }
}

// The details panel only builds a row for a visible property, and a binding list is never typed into.
TEST(ScriptDelegateReflection, DelegatePropertiesAreVisibleAndReadOnly)
{
    const FDelegateProperty* Slot = FindDelegate("OnTwo");
    EXPECT_TRUE(Slot->IsVisible());
    EXPECT_TRUE(EnumHasAnyFlags(Slot->Flags, EPropertyFlags::ReadOnly));
    EXPECT_FALSE(EnumHasAnyFlags(Slot->Flags, EPropertyFlags::Editable));
}

// The row shows natives and scripts apart, which it derives by subtracting one count from the other.
TEST(ScriptDelegateReflection, BindingCountsSeparateNativeFromManaged)
{
    SDelegateTestHost Host;
    EXPECT_EQ(Host.OnOne.GetBindingCount(), 0u);

    const FDelegateHandle First = Host.OnOne.AddLambda([](const SDelegateTestPayload&) {});
    Host.OnOne.AddLambda([](const SDelegateTestPayload&) {});

    EXPECT_EQ(Host.OnOne.GetBindingCount(), 2u);
    EXPECT_EQ(Host.OnOne.GetManagedBindingCount(), 0u);

    Host.OnOne.Remove(First);
    EXPECT_EQ(Host.OnOne.GetBindingCount(), 1u);
}
