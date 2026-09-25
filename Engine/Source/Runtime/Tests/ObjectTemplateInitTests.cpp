#include <gtest/gtest.h>

#include "Containers/Name.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

namespace
{
    // Every test here edits a shared CDO, so each one puts it back or the next reads the leftovers.
    template<typename T, typename TValue>
    class TScopedDefault
    {
    public:

        TScopedDefault(TValue T::* InMember, TValue NewValue)
            : Member(InMember)
            , Previous(GetMutableDefault<T>()->*InMember)
        {
            GetMutableDefault<T>()->*Member = Move(NewValue);
        }

        ~TScopedDefault()
        {
            GetMutableDefault<T>()->*Member = Move(Previous);
        }

        LE_NO_COPYMOVE(TScopedDefault);

    private:

        TValue T::* Member;
        TValue      Previous;
    };

    CScriptableTest* NewTransientScriptable()
    {
        return NewObject<CScriptableTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    }
}

TEST(ObjectTemplateInit, NewObjectTakesScalarDefaultsFromTheCDO)
{
    const float Constructed = NewTransientScriptable()->NativeValue;
    ASSERT_FLOAT_EQ(Constructed, 1.5f) << "the fixture's C++ initializer changed; the rest of this test is meaningless";

    TScopedDefault<CScriptableTest, float> Default(&CScriptableTest::NativeValue, 42.0f);

    EXPECT_FLOAT_EQ(NewTransientScriptable()->NativeValue, 42.0f)
        << "a new object ignored the value its class declares as the default";
}

TEST(ObjectTemplateInit, NewObjectTakesContainerDefaultsFromTheCDO)
{
    TVector<FName> Seeded;
    Seeded.push_back(FName("Alpha"));
    Seeded.push_back(FName("Beta"));

    TScopedDefault<CEntityScriptTest, TVector<FName>> Default(&CEntityScriptTest::Values, Seeded);

    CEntityScriptTest* Fresh = NewObject<CEntityScriptTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);

    ASSERT_EQ(Fresh->Values.size(), 2u) << "a container default did not reach the new object";
    EXPECT_EQ(Fresh->Values[0], FName("Alpha"));
    EXPECT_EQ(Fresh->Values[1], FName("Beta"));

    // Proves the copy is a real one rather than a shared buffer, which a memcpy of the header would give.
    Fresh->Values.push_back(FName("Gamma"));
    EXPECT_EQ(GetMutableDefault<CEntityScriptTest>()->Values.size(), 2u) << "the new object writes through to the CDO";
}

TEST(ObjectTemplateInit, AnExplicitTemplateBeatsTheCDO)
{
    TScopedDefault<CScriptableTest, float> Default(&CScriptableTest::NativeValue, 42.0f);

    CScriptableTest* Archetype = NewTransientScriptable();
    Archetype->NativeValue = 7.0f;

    CScriptableTest* Fresh = NewObjectFromTemplate<CScriptableTest>(Archetype, nullptr, NAME_None, FGuid::New(), OF_Transient);

    ASSERT_NE(Fresh, nullptr);
    EXPECT_FLOAT_EQ(Fresh->NativeValue, 7.0f) << "the explicit template was ignored in favor of the CDO";
}

TEST(ObjectTemplateInit, TheCDOItselfKeepsItsConstructorValues)
{
    // Discarding forces the next request to build a fresh one, which must not seed from the outgoing copy.
    CClass* Class = CScriptableTest::StaticClass();
    GetMutableDefault<CScriptableTest>()->NativeValue = 42.0f;
    Class->DiscardDefaultObject();

    EXPECT_FLOAT_EQ(GetMutableDefault<CScriptableTest>()->NativeValue, 1.5f)
        << "a rebuilt CDO seeded itself from something other than its own constructor";
}

TEST(ObjectConstruction, AConstructorSeesItsOwnClassNameAndPackage)
{
    CPackage* Package = FindObject<CPackage>("/Script/Engine");
    ASSERT_NE(Package, nullptr) << "no package to construct into, so the assertion below proves nothing";

    CConstructorIdentityTest* Object = NewObject<CConstructorIdentityTest>(
        Package, FName("IdentityProbe"), FGuid::New(), OF_Transient);

    ASSERT_NE(Object, nullptr);
    EXPECT_EQ(Object->SeenClass, CConstructorIdentityTest::StaticClass()) << "GetClass was null inside the constructor";
    EXPECT_EQ(Object->SeenName, FName("IdentityProbe")) << "GetName was unset inside the constructor";
    EXPECT_EQ(Object->SeenPackage, Package) << "GetPackage was null inside the constructor";
}
