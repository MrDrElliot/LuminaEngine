#include <gtest/gtest.h>

#include "Core/Object/Class.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/PropertyFlags.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Paths/Paths.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/ManagedTypeRegistry.h"
#include "Scripting/ScriptTypeRegistry.h"

namespace LuminaScriptSoftReferenceListTests
{
    using namespace Lumina;

    using FLoadSourceFn = int32(*)(const char*, int32);

    constexpr const char* kSource = R"(
        using Lumina;
        using LuminaSharp;

        [Reflect]
        public partial struct SoftReferenceListProbe
        {
            [Property] public TVector<TSoftObjectPtr<CTexture>> Textures;
            [Property] public THashMap<int, FSoftObjectPath> AssetsBySlot;
        }
    )";

    class FScriptSoftReferenceListTest : public ::testing::Test
    {
    public:

        static void SetUpTestSuite()
        {
            Paths::InitializePaths();
            DotNet::Initialize();
        }

        static void TearDownTestSuite()
        {
            if (DotNet::IsInitialized())
            {
                LoadScripts(FString());
                DotNet::Shutdown();
            }
        }

    protected:

        void SetUp() override
        {
            if (!DotNet::IsInitialized())
            {
                GTEST_SKIP() << "the managed host did not start in this process";
            }
            RegisterPendingTypes();
        }

        static bool LoadScripts(const FString& Source)
        {
            auto* Load = (FLoadSourceFn)DotNet::ResolveManagedExport("Test_LoadScriptSource");
            if (Load == nullptr)
            {
                return false;
            }
            Scripting::RegisterBuiltInManagedTypeStages();
            Scripting::FManagedTypeRegistry::Get().PreUnloadAll();
            if (Load(*Source, Source.Len()) != 0)
            {
                Scripting::FManagedTypeRegistry::Get().UnloadAbortedAll();
                return false;
            }
            TVector<Scripting::FManagedTypeDefinition> Definitions;
            TVector<Scripting::FManagedTypeAlias> Aliases;
            DotNet::GatherManagedTypeDefinitions(Definitions, Aliases);
            Scripting::FManagedTypeRegistry::Get().CompileAll(Definitions, Aliases);
            return true;
        }
    };

    TEST_F(FScriptSoftReferenceListTest, AScriptCanHoldAListOfAssetReferences)
    {
        ASSERT_TRUE(LoadScripts(kSource)) << "a container of asset references compiles";

        CStruct* Probe = ResolveStructByName(FName("SoftReferenceListProbe"));
        ASSERT_NE(Probe, nullptr);

        FProperty* Textures = Probe->GetProperty(FName("Textures"));
        ASSERT_NE(Textures, nullptr);
        ASSERT_EQ(Textures->GetType(), EPropertyTypeFlags::Vector);
        EXPECT_EQ(static_cast<FArrayProperty*>(Textures)->GetInternalProperty()->GetType(), EPropertyTypeFlags::SoftObject);

        FProperty* AssetsBySlot = Probe->GetProperty(FName("AssetsBySlot"));
        ASSERT_NE(AssetsBySlot, nullptr);
        EXPECT_EQ(AssetsBySlot->GetType(), EPropertyTypeFlags::Map);
    }
}
