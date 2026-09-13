#include <gtest/gtest.h>

#include "Core/Object/Cast.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

// The managed side reaches these by name, so the tests call them the same way rather than through a wrapper.
extern "C" void* LuminaSharp_NewObject(void* Class, void* Package, const char* Name, int NameLen);
extern "C" void* LuminaSharp_FindClassByName(const char* Name, int Len);

namespace
{
    void* FindTestClass()
    {
        static const bool bReady = [] { ProcessNewlyLoadedCObjects(); return true; }();
        (void)bReady;

        const char* Name = "CScriptableTest";
        return LuminaSharp_FindClassByName(Name, (int)strlen(Name));
    }
}

TEST(ScriptObjectFactory, AClassResolvesByName)
{
    EXPECT_EQ(FindTestClass(), CScriptableTest::StaticClass());
}

TEST(ScriptObjectFactory, ScriptCanConstructAnObject)
{
    void* Class = FindTestClass();
    ASSERT_NE(Class, nullptr);

    CObject* Made = static_cast<CObject*>(LuminaSharp_NewObject(Class, nullptr, nullptr, 0));
    ASSERT_NE(Made, nullptr);

    EXPECT_NE(Cast<CScriptableTest>(Made), nullptr) << "the object is of the class that was asked for";
    EXPECT_EQ(Made->GetClass(), CScriptableTest::StaticClass());

    Made->ConditionalBeginDestroy();
}

// Nothing a script makes should land in something that gets saved unless it says otherwise.
TEST(ScriptObjectFactory, AnObjectWithNoPackageGoesToTheTransientOne)
{
    void* Class = FindTestClass();
    ASSERT_NE(Class, nullptr);

    CObject* Made = static_cast<CObject*>(LuminaSharp_NewObject(Class, nullptr, nullptr, 0));
    ASSERT_NE(Made, nullptr);

    EXPECT_EQ(Made->GetPackage(), CPackage::GetTransientPackage());
    EXPECT_TRUE(Made->GetPackage()->IsTransientPackage());

    Made->ConditionalBeginDestroy();
}

TEST(ScriptObjectFactory, ANamedObjectKeepsTheNameItWasGiven)
{
    void* Class = FindTestClass();
    ASSERT_NE(Class, nullptr);

    const char* Name = "ScriptMadeThis";
    CObject* Made = static_cast<CObject*>(LuminaSharp_NewObject(Class, nullptr, Name, (int)strlen(Name)));
    ASSERT_NE(Made, nullptr);

    EXPECT_EQ(Made->GetName(), FName(Name));

    Made->ConditionalBeginDestroy();
}

TEST(ScriptObjectFactory, AnUnnamedObjectGetsAUniqueName)
{
    void* Class = FindTestClass();
    ASSERT_NE(Class, nullptr);

    CObject* First = static_cast<CObject*>(LuminaSharp_NewObject(Class, nullptr, nullptr, 0));
    CObject* Second = static_cast<CObject*>(LuminaSharp_NewObject(Class, nullptr, nullptr, 0));
    ASSERT_NE(First, nullptr);
    ASSERT_NE(Second, nullptr);

    EXPECT_NE(First->GetName(), Second->GetName()) << "two unnamed objects must not collide";

    First->ConditionalBeginDestroy();
    Second->ConditionalBeginDestroy();
}

TEST(ScriptObjectFactory, ANullClassIsRefusedRatherThanCrashing)
{
    EXPECT_EQ(LuminaSharp_NewObject(nullptr, nullptr, nullptr, 0), nullptr);
}

// Every generated class carries a factory, so the export's factory check has nothing to refuse here. Left
// asserted the other way round, so that if a class ever does arrive without one this stops being silent.
TEST(ScriptObjectFactory, EveryClassAScriptCanNameCanBeInstantiated)
{
    CClass* Class = static_cast<CClass*>(FindTestClass());
    ASSERT_NE(Class, nullptr);
    EXPECT_NE(Class->FactoryFunction, nullptr);
}
