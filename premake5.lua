
-- Setup action runs without the engine tree; load it first and bail out before workspace evaluation.
include "BuildScripts/Actions/Setup"
if _ACTION == "setup" then return end

include "BuildScripts/Dependencies"
include "BuildScripts/Workspace"
include "BuildScripts/CSharpProject"
include "BuildScripts/Module"
include "BuildScripts/PluginDiscovery"
include "BuildScripts/Actions/Reflection"
include "BuildScripts/Actions/Clean"

-- Tests + GoogleTest off by default (~22s clean-build cost); enable with --with-tests.
newoption {
    trigger     = "with-tests",
    description = "Include the Tests project and its GoogleTest dependency",
}

LuminaWorkspaceSettings({
    Name         = "Lumina",
    StartProject = "Lumina",
    TargetDir    = LuminaConfig.GetTargetDirectory(),
    ObjDir       = LuminaConfig.GetObjDirectory(),
})

    group "Engine"
		include "Engine/Source/Runtime"
        include "Engine/Editor"
	group ""
    -- Sandbox is a standalone game project (own Sandbox.slnx, links the pre-built engine), so it is intentionally not in the engine workspace.

    group "Engine/Managed"
        -- [NativeCall] Roslyn source generator.
        project "LuminaSharp.Generators"
            location "Engine/Source/LuminaSharp.Generators"
            kind "SharedLib"
            language "C#"
            clr "NetCore"
            dotnetsdk "Default"
            dotnetframework "netstandard2.0"
            files { "Engine/Source/LuminaSharp.Generators/**.cs" }
            -- Keep obj/bin out or they duplicate the SDK's auto-generated sources.
            removefiles { "Engine/Source/LuminaSharp.Generators/obj/**", "Engine/Source/LuminaSharp.Generators/bin/**" }
            dotnetstripdefines(true)
            dotnetrawprops {
                "<LangVersion>latest</LangVersion>",
                "<Nullable>enable</Nullable>",
                "<ImplicitUsings>disable</ImplicitUsings>",
                "<IncludeBuildOutput>false</IncludeBuildOutput>",
                "<IsRoslynComponent>true</IsRoslynComponent>",
                "<EnforceExtendedAnalyzerRules>true</EnforceExtendedAnalyzerRules>",
            }
            -- PrivateAssets=all keeps the Roslyn package build-time only.
            dotnetrawitems {
                [[<PackageReference Include="Microsoft.CodeAnalysis.CSharp" Version="4.13.0" PrivateAssets="all" />]],
            }

        -- Managed engine API (loaded at runtime by FDotNetHost).
        project "LuminaSharp"
            location "Engine/Source/LuminaSharp"
            kind "SharedLib"
            language "C#"
            clr "NetCore"
            dotnetsdk "Default"
            dotnetframework "net10.0"
            files { "Engine/Source/LuminaSharp/**.cs" }
            removefiles { "Engine/Source/LuminaSharp/obj/**", "Engine/Source/LuminaSharp/bin/**" }
            -- Must build after Runtime (Reflector emits the bindings) and after the generator (analyzer DLL exists).
            dependson { "Runtime", "LuminaSharp.Generators" }
            targetdir(path.join(LuminaConfig.GetTargetDirectory(), "DotNet", "Managed"))
            dotnetstripdefines(true)
            dotnetrawprops {
                "<EnableDynamicLoading>true</EnableDynamicLoading>",
                "<AllowUnsafeBlocks>true</AllowUnsafeBlocks>",
                "<Nullable>enable</Nullable>",
                "<ImplicitUsings>disable</ImplicitUsings>",
                "<AssemblyName>LuminaSharp</AssemblyName>",
                "<RootNamespace>LuminaSharp</RootNamespace>",
                "<Deterministic>true</Deterministic>",
                "<AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>",
                "<AppendRuntimeIdentifierToOutputPath>false</AppendRuntimeIdentifierToOutputPath>",
            }
            dotnetrawitems {
                [[<PackageReference Include="Microsoft.CodeAnalysis.CSharp" Version="4.13.0" />]],
                -- Generator referenced as an analyzer.
                [[<ProjectReference Include="..\LuminaSharp.Generators\LuminaSharp.Generators.csproj" OutputItemType="Analyzer" ReferenceOutputAssembly="false" />]],
                -- Reflector-emitted bindings (MSBuild glob, expanded at build time since they don't exist at premake time).
                [[<Compile Include="..\..\..\Intermediates\CSharpBindings\**\*.generated.cs"><Link>Generated\%(RecursiveDir)%(Filename)%(Extension)</Link></Compile>]],
            }
            -- Copy the generator next to LuminaSharp.dll so the runtime ScriptCompiler can load it.
            dotnetrawtail {
                [[<Target Name="CopyGeneratorForRuntime" AfterTargets="Build">]],
                [[  <Copy SourceFiles="@(Analyzer)" DestinationFolder="$(OutputPath)" SkipUnchangedFiles="true" Condition="'%(Filename)' == 'LuminaSharp.Generators'" />]],
                [[</Target>]],
            }
    group ""

    -- Must come after Engine so Tests' ModuleDependencies (Runtime/Editor) are registered and propagate their include dirs.
    if _OPTIONS["with-tests"] then
        group "Tests"
            include "Engine/Tests"
        group ""
    end

    -- Engine plugins drop in automatically: Engine/Plugins/<Name>/<Name>.lua becomes a project here.
    group "Plugins"
        LuminaDiscoverEnginePlugins()
    group ""

    group "Applications"
    	include "Engine/Applications/Lumina"
		include "Engine/Applications/Reflector"
	group ""

    group "Engine/Shaders"
        include "Engine/Resources/Shaders"
    group ""

	group "Engine/ThirdParty"
        include "Engine/Source/ThirdParty/EA"
        include "Engine/Source/ThirdParty/EnTT"
		include "Engine/Source/ThirdParty/glfw"
		include "Engine/Source/ThirdParty/imgui"
        if LuminaOptions.IsActiveAny("Tracy") then
            include "Engine/Source/ThirdParty/Tracy"
        end
        include "Engine/Source/ThirdParty/MiniAudio"
        include "Engine/Source/ThirdParty/JoltPhysics"
        include "Engine/Source/ThirdParty/Recast"
        include "Engine/Source/ThirdParty/enet"
        include "Engine/Source/ThirdParty/RPMalloc"
        include "Engine/Source/ThirdParty/XXHash"
        include "Engine/Source/ThirdParty/miniz"
        include "Engine/Source/ThirdParty/VulkanMemoryAllocator"
        include "Engine/Source/ThirdParty/Volk"
        include "Engine/Source/ThirdParty/tinyobjloader"
        include "Engine/Source/ThirdParty/MeshOptimizer"
        include "Engine/Source/ThirdParty/MikkTSpace"
        include "Engine/Source/ThirdParty/json"
        include "Engine/Source/ThirdParty/fastgltf"
        include "Engine/Source/ThirdParty/OpenFBX"
        include "Engine/Source/ThirdParty/basis_universal"
        include "Engine/Source/ThirdParty/SLang"
        if _OPTIONS["with-tests"] then
            include "Engine/Source/ThirdParty/GoogleTest"
        end
        include "Engine/Source/ThirdParty/FreeType"
        include "Engine/Source/ThirdParty/RmlUi"
        include "Engine/Source/ThirdParty/msdfgen"
	group ""

    group "Build"
        include "BuildScripts"
        include "BuildScripts/ReflectionGen.lua"
    group ""