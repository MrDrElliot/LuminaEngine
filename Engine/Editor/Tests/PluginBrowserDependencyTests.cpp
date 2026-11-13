#include <gtest/gtest.h>

#include "Core/Plugin/Plugin.h"
#include "UI/Tools/PluginBrowserEditorTool.h"

using namespace Lumina;

namespace
{
    FPlugin MakePlugin(const char* Name, bool bEnabled, TVector<FPluginDependency> Dependencies = {})
    {
        FPluginDescriptor Desc;
        Desc.Name = Name;
        Desc.Dependencies = Move(Dependencies);
        FPlugin Plugin(Move(Desc), FString(), FString());
        Plugin.SetEnabled(bEnabled);
        return Plugin;
    }

    FPluginDependency Requires(const char* Name, bool bOptional = false)
    {
        FPluginDependency Dep;
        Dep.Name = Name;
        Dep.bOptional = bOptional;
        return Dep;
    }

    TVector<FString> Broken(const TVector<const FPlugin*>& Plugins, const THashMap<FName, bool>& Pending)
    {
        return FPluginBrowserEditorTool::FindBrokenDependencies(Plugins, Pending);
    }
}

TEST(PluginBrowserDependencies, DisablingARequiredDependencyOfAnEnabledPluginIsRejected)
{
    const FPlugin Base = MakePlugin("Base", true);
    const FPlugin User = MakePlugin("User", true, { Requires("Base") });

    THashMap<FName, bool> Pending;
    Pending[FName("Base")] = false;

    const TVector<FString> Result = Broken({ &Base, &User }, Pending);
    ASSERT_EQ(Result.Num(), 1);
    EXPECT_NE(Result[0].Find("'User' requires 'Base'"), Constants::kIndexNone);
}

TEST(PluginBrowserDependencies, DisablingTheDependentToo_Applies)
{
    const FPlugin Base = MakePlugin("Base", true);
    const FPlugin User = MakePlugin("User", true, { Requires("Base") });

    THashMap<FName, bool> Pending;
    Pending[FName("Base")] = false;
    Pending[FName("User")] = false;

    EXPECT_TRUE(Broken({ &Base, &User }, Pending).IsEmpty());
}

TEST(PluginBrowserDependencies, DisablingWithOnlyOptionalOrDisabledDependents_Applies)
{
    const FPlugin Base = MakePlugin("Base", true);
    const FPlugin Optional = MakePlugin("Optional", true, { Requires("Base", true) });
    const FPlugin Off = MakePlugin("Off", false, { Requires("Base") });

    THashMap<FName, bool> Pending;
    Pending[FName("Base")] = false;

    EXPECT_TRUE(Broken({ &Base, &Optional, &Off }, Pending).IsEmpty());
}

TEST(PluginBrowserDependencies, EnablingWhileARequiredDependencyStaysDisabledIsRejected)
{
    const FPlugin Base = MakePlugin("Base", false);
    const FPlugin User = MakePlugin("User", false, { Requires("Base") });

    THashMap<FName, bool> Pending;
    Pending[FName("User")] = true;

    const TVector<FString> Result = Broken({ &Base, &User }, Pending);
    ASSERT_EQ(Result.Num(), 1);
    EXPECT_NE(Result[0].Find("'User' requires 'Base'"), Constants::kIndexNone);

    Pending[FName("Base")] = true;
    EXPECT_TRUE(Broken({ &Base, &User }, Pending).IsEmpty());
}

TEST(PluginBrowserDependencies, EnablingWithAMissingRequiredDependencyIsRejected)
{
    const FPlugin User = MakePlugin("User", false, { Requires("Gone") });

    THashMap<FName, bool> Pending;
    Pending[FName("User")] = true;

    EXPECT_EQ(Broken({ &User }, Pending).Num(), 1);
}

TEST(PluginBrowserDependencies, PreexistingBreaksDoNotBlockUnrelatedToggles)
{
    const FPlugin Base = MakePlugin("Base", false);
    const FPlugin User = MakePlugin("User", true, { Requires("Base") });
    const FPlugin Other = MakePlugin("Other", true);

    THashMap<FName, bool> Pending;
    Pending[FName("Other")] = false;

    EXPECT_TRUE(Broken({ &Base, &User, &Other }, Pending).IsEmpty());
}
