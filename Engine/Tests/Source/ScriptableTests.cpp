#include <gtest/gtest.h>

#include "Containers/Name.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

// Verifies the native half of the Scriptable pipeline WITHOUT the managed host: the Reflector-generated shim
// registers CScriptableTest, we mint a real CClass subclass from it (the same path a C# subclass takes), and
// confirm discovery + instantiation + vtable. With no managed override bound, OnTest runs the C++ default.
// (The full C# override -> 105 path needs the .NET host + a loaded C# subclass; see `script.scriptable_selftest`.)
TEST(Scriptable, MintInstantiateAndNativeDefaultDispatch)
{
    CClass* Base = CScriptableTest::StaticClass();
    ASSERT_NE(Base, nullptr);

    CClass* Sub = FScriptableRegistry::Mint("ScriptableTest_GTestSub", "CScriptableTest");
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

    // Object arg + return marshalling: the C++ default echoes its input. Exercises the shim's object signature
    // (CWorld* in/out); a non-null sentinel proves the arg passes through to the return rather than being dropped.
    CWorld* const Sentinel = reinterpret_cast<CWorld*>(0x1234);
    EXPECT_EQ(Inst->OnEchoWorld(Sentinel), Sentinel);
    EXPECT_EQ(Inst->OnEchoWorld(nullptr), nullptr);
}
