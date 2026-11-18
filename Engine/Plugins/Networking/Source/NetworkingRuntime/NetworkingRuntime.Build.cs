using LuminaBuildTool.Configuration;

public class NetworkingRuntime : LuminaModuleRules
{
    public NetworkingRuntime(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        HostType = ModuleHostType.Runtime;

        PublicIncludePaths.Add("..");

        PublicDependencyModuleNames.Add("Runtime");

        // The backend this module exists to wrap. Nothing outside it links ENet.
        PrivateDependencyModuleNames.Add("ENet");

        // ENet brings in windows.h, whose macros such as DrawText would rename calls in any file batched after it.
        ExcludeFromUnity.Add("ENetTransport.cpp");
        ExcludeFromUnity.Add("NetworkSubsystem.cpp");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ImGui",
            "RPMalloc",
            "Tracy",
        });
    }
}
