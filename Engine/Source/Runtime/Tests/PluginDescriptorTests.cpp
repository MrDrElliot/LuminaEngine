#include <gtest/gtest.h>

#include "Core/Plugin/PluginDescriptor.h"
#include "Containers/StringFormat.h"

namespace LuminaPluginDescriptorTests
{
    using Lumina::FPluginDescriptor;
    using Lumina::FString;

    // Summaries are LuminaBuildTool's PluginDescriptor.Load output for the same text; null means both reject it.
    struct FFixture
    {
        const char* Label;
        const char* Json;
        const char* Summary;
    };

    constexpr FFixture kFixtures[] =
    {
        { "minimal", R"lplugin({ "Name": "P" })lplugin", "P|101|1||" },
        { "string dependency", R"lplugin({ "Name": "P", "Dependencies": [ "Networking" ] })lplugin", "P|101|1|Networking/0/0|" },
        { "object dependency", R"lplugin({ "Name": "P", "Dependencies": [ { "Name": "Networking", "Optional": true, "Version": 2 } ] })lplugin", "P|101|1|Networking/1/2|" },
        { "mixed dependencies", R"lplugin({ "Name": "P", "Dependencies": [ "A", { "Name": "B" } ] })lplugin", "P|101|1|A/0/0,B/0/0|" },
        { "module defaults", R"lplugin({ "Name": "P", "Modules": [ { "Name": "PRuntime" } ] })lplugin", "P|101|1||PRuntime/Runtime/PreEngineInit/" },
        { "module platforms", R"lplugin({ "Name": "P", "Modules": [ { "Name": "PRuntime", "Type": "Editor", "LoadingPhase": "EditorInit", "SupportedPlatforms": [ "Linux" ] } ] })lplugin", "P|101|1||PRuntime/Editor/EditorInit/Linux" },
        { "reflection module", R"lplugin({ "Name": "P", "Modules": [ { "Name": "PGenerator", "Type": "Reflection" } ] })lplugin", "P|101|1||PGenerator/Reflection/PreEngineInit/" },
        { "unknown key ignored", R"lplugin({ "Name": "P", "CookRoots": [ "/Game/Map" ], "Whatever": 3 })lplugin", "P|101|1||" },
        { "missing name", R"lplugin({ "Version": 1 })lplugin", nullptr },
        { "empty name", R"lplugin({ "Name": "" })lplugin", nullptr },
        { "wrong bool type", R"lplugin({ "Name": "P", "EnabledByDefault": "yes" })lplugin", nullptr },
        { "wrong int type", R"lplugin({ "Name": "P", "Version": "1" })lplugin", nullptr },
        { "null string", R"lplugin({ "Name": "P", "Author": null })lplugin", nullptr },
        { "dependency number", R"lplugin({ "Name": "P", "Dependencies": [ 7 ] })lplugin", nullptr },
        { "dependency no name", R"lplugin({ "Name": "P", "Dependencies": [ { "Optional": true } ] })lplugin", nullptr },
        { "dependency bad optional", R"lplugin({ "Name": "P", "Dependencies": [ { "Name": "A", "Optional": 1 } ] })lplugin", nullptr },
        { "unknown module type", R"lplugin({ "Name": "P", "Modules": [ { "Name": "M", "Type": "Program" } ] })lplugin", nullptr },
        { "unknown loading phase", R"lplugin({ "Name": "P", "Modules": [ { "Name": "M", "LoadingPhase": "Default" } ] })lplugin", nullptr },
        { "module no name", R"lplugin({ "Name": "P", "Modules": [ { "Type": "Runtime" } ] })lplugin", nullptr },
        { "platform not string", R"lplugin({ "Name": "P", "SupportedPlatforms": [ 1 ] })lplugin", nullptr },
        { "wrong key case", R"lplugin({ "name": "P" })lplugin", nullptr },
        { "comment", R"lplugin({ "Name": "P" // note
 })lplugin", nullptr },
        { "trailing comma", R"lplugin({ "Name": "P", })lplugin", nullptr },
    };

    FString Summarize(const FPluginDescriptor& Descriptor)
    {
        FString Dependencies;
        for (const Lumina::FPluginDependency& Dependency : Descriptor.Dependencies)
        {
            if (!Dependencies.IsEmpty())
            {
                Dependencies += ",";
            }
            Dependencies += Lumina::Format("{}/{}/{}", Dependency.Name, Dependency.bOptional ? 1 : 0, Dependency.Version);
        }

        FString Modules;
        for (const Lumina::FPluginModuleDescriptor& Module : Descriptor.Modules)
        {
            if (!Modules.IsEmpty())
            {
                Modules += ",";
            }
            FString Platforms;
            for (const FString& Platform : Module.SupportedPlatforms)
            {
                if (!Platforms.IsEmpty())
                {
                    Platforms += "+";
                }
                Platforms += Platform;
            }
            Modules += Lumina::Format("{}/{}/{}/{}", Module.Name, Lumina::LexToString(Module.Type), Lumina::LexToString(Module.LoadingPhase), Platforms);
        }

        return Lumina::Format("{}|{}{}{}|{}|{}|{}", Descriptor.Name, Descriptor.bEnabledByDefault ? 1 : 0, Descriptor.bEditorOnly ? 1 : 0,
            Descriptor.bContainsContent ? 1 : 0, Descriptor.Version, Dependencies, Modules);
    }

    TEST(PluginDescriptor, ReadsEveryFixtureExactlyAsTheBuildToolDoes)
    {
        for (const FFixture& Fixture : kFixtures)
        {
            FPluginDescriptor Descriptor;
            FString Error;
            const bool bLoaded = FPluginDescriptor::LoadFromString(Fixture.Json, Descriptor, Error);
            if (Fixture.Summary == nullptr)
            {
                EXPECT_FALSE(bLoaded) << Fixture.Label << ": the build tool rejects this descriptor";
                EXPECT_FALSE(Error.IsEmpty()) << Fixture.Label;
            }
            else
            {
                ASSERT_TRUE(bLoaded) << Fixture.Label << ": " << *Error;
                EXPECT_EQ(Summarize(Descriptor), Fixture.Summary) << Fixture.Label;
            }
        }
    }
}
