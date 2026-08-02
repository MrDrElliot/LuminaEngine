using LuminaBuildTool.Configuration;

public class ENet : LuminaThirdPartyModuleRules
{
    public ENet(TargetInfo Target)
        : base(Target)
    {
        bCompileAsC = true;
        PublicIncludePaths.Add("include");

        bUseExplicitSourceList = true;
        ExtraSourceFiles.AddRange(new[]
        {
            "callbacks.c",
            "compress.c",
            "host.c",
            "list.c",
            "packet.c",
            "peer.c",
            "protocol.c",
        });

        if (Target.Platform == BuildPlatform.Windows64)
        {
            ExtraSourceFiles.Add("win32.c");
            PrivateDefinitions.Add("_WINSOCK_DEPRECATED_NO_WARNINGS");

            // Winsock plus the multimedia timer ENet uses for its time base.
            PublicSystemLibraries.Add("ws2_32");
            PublicSystemLibraries.Add("winmm");
        }
        else
        {
            ExtraSourceFiles.Add("unix.c");
            PrivateDefinitions.Add("HAS_SOCKLEN_T");
        }
    }
}
