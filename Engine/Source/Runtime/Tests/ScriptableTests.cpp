#include <gtest/gtest.h>

#include "Containers/Name.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ManagedInstance.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

// Verifies the native half without the managed host, so OnTest runs the C++ default.
TEST(Scriptable, MintInstantiateAndNativeDefaultDispatch)
{
    CClass* Base = CScriptableTest::StaticClass();
    ASSERT_NE(Base, nullptr);

    CClass* Sub = FScriptableRegistry::Mint("ScriptableTest_GTestSub", "CScriptableTest", 0);
    ASSERT_NE(Sub, nullptr) << "minting failed (is the CScriptableTest shim registered?)";
    ProcessNewlyLoadedCObjects(); // finalize registration + CDO so FindObject/NewObject work

    EXPECT_TRUE(Sub->IsChildOf(Base));
    EXPECT_EQ(FindObject<CClass>(FName("ScriptableTest_GTestSub")), Sub);

    CObject* Obj = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Obj, nullptr);
    CScriptableTest* Inst = Cast<CScriptableTest>(Obj);
    ASSERT_NE(Inst, nullptr) << "minted instance is not a CScriptableTest (wrong shim vtable / base)";

    // No managed instance bound (host not running in the test) -> the shim falls through to the C++ default.
    EXPECT_EQ(Inst->OnTest(5), 10);

    // A non-null sentinel proves the object arg passes through to the return.
    CWorld* const Sentinel = reinterpret_cast<CWorld*>(0x1234);
    EXPECT_EQ(Inst->OnEchoWorld(Sentinel), Sentinel);
    EXPECT_EQ(Inst->OnEchoWorld(nullptr), nullptr);
}

// The override mask lives on the minted CClass and the managed instance in the object's slot.

TEST(Scriptable, MintStampsTheOverrideMaskOnTheClass)
{
    constexpr uint64 Mask = (1ull << 0) | (1ull << 3);

    CClass* Sub = FScriptableRegistry::Mint("ScriptableTest_GTestMask", "CScriptableTest", Mask);
    ASSERT_NE(Sub, nullptr);
    ProcessNewlyLoadedCObjects();
    Sub->GetDefaultObject();

    EXPECT_EQ(Sub->ScriptOverrides, Mask) << "the mask a C# subclass reported was not stamped on its class";
    EXPECT_EQ(CScriptableTest::StaticClass()->ScriptOverrides, 0ull)
        << "a native class must carry an empty mask, so its shim never even looks for a managed instance";
}

// With the bit set but no managed instance, the shim must fall through to the C++ default.
TEST(Scriptable, DispatchFallsBackToNativeWhenNoManagedInstanceExists)
{
    CClass* Sub = FScriptableRegistry::Mint("ScriptableTest_GTestDispatch", "CScriptableTest", 1ull << 0);
    ASSERT_NE(Sub, nullptr);
    ProcessNewlyLoadedCObjects();
    Sub->GetDefaultObject();
    ASSERT_EQ(Sub->ScriptOverrides, 1ull << 0);

    CObject* Object = NewObject(Sub, nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Object, nullptr);
    CScriptableTest* Typed = Cast<CScriptableTest>(Object);
    ASSERT_NE(Typed, nullptr);

    const int32 LiveBefore = ManagedInstances::GetLiveCount();

    EXPECT_EQ(Typed->OnTest(5), 10) << "override bit set + no managed instance must fall back to the C++ default";
    EXPECT_EQ(Typed->OnTest(7), 14) << "the fallback must be stable across repeated dispatches";

    EXPECT_EQ(ManagedInstances::GetLiveCount(), LiveBefore)
        << "a failed instance creation must not occupy a slot (it would leak one per object)";

    Object->ForceDestroyNow();
    EXPECT_EQ(ManagedInstances::GetLiveCount(), LiveBefore);
}

// The class default object must never acquire a managed counterpart.
TEST(Scriptable, DefaultObjectNeverGetsAManagedInstance)
{
    CClass* Sub = FScriptableRegistry::Mint("ScriptableTest_GTestCdo", "CScriptableTest", 1ull << 0);
    ASSERT_NE(Sub, nullptr);
    ProcessNewlyLoadedCObjects();

    CObject* Cdo = Sub->GetDefaultObject();
    ASSERT_NE(Cdo, nullptr);
    ASSERT_TRUE(Cdo->HasAnyFlag(OF_DefaultObject));

    const int32 LiveBefore = ManagedInstances::GetLiveCount();

    EXPECT_EQ(Scriptable::GetOrCreateInstance(Cdo), nullptr) << "the CDO must never bind a managed instance";
    EXPECT_EQ(ManagedInstances::Find(Cdo), nullptr);
    EXPECT_EQ(ManagedInstances::GetLiveCount(), LiveBefore);
}
