#include <gtest/gtest.h>

#include "Config/Config.h"
#include "Config/EngineSettings.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectFlags.h"
#include "Core/Object/ObjectHandleTyped.h"

using namespace Lumina;

// A dropped settings class must not leave a null slot for the next settings pass to deref.
TEST(Config, VisitObjectReferencesDropsASettingsClassThatFailsToRemap)
{
    CClass* KeptClass = CRenderLayerSettings::StaticClass();
    CClass* RemovedClass = CCollisionSettings::StaticClass();
    ASSERT_NE(KeptClass, nullptr);
    ASSERT_NE(RemovedClass, nullptr);
    ASSERT_NE(KeptClass->GetDefaultObject(), nullptr);
    ASSERT_NE(RemovedClass->GetDefaultObject(), nullptr);

    FConfig TestConfig;
    TestConfig.DiscoverAndLoadSettings();

    // Pinned because unrooting drops the last strong reference and would free it before the check below.
    TStrongObjectPtr<CObject> RemovedSnapshot(TestConfig.GetSettingsDefault(RemovedClass));
    ASSERT_NE(RemovedSnapshot.Get(), nullptr);
    EXPECT_TRUE(RemovedSnapshot->HasAnyFlag(OF_Rooted));

    TestConfig.VisitObjectReferences([&](CObject* Object) -> CObject*
    {
        return Object == RemovedClass ? nullptr : Object;
    });

    EXPECT_FALSE(RemovedSnapshot->HasAnyFlag(OF_Rooted)) << "a snapshot whose class failed to remap must be unrooted, not leaked";
    EXPECT_EQ(TestConfig.GetSettingsDefault(RemovedClass), nullptr);

    int32 VisitCount = 0;
    TestConfig.ForEachSettingsClass([&](CClass* Class)
    {
        ASSERT_NE(Class, nullptr) << "DiscoveredSettings must never hold a null entry after a failed remap";
        Class->HasMeta("ConfigFile");
        ++VisitCount;
    });
    EXPECT_GT(VisitCount, 0);

    TestConfig.DiscoverAndLoadSettings();
    EXPECT_NE(TestConfig.GetSettingsDefault(KeptClass), nullptr) << "the surviving class still loads";
}
