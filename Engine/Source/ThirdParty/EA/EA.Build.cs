using LuminaBuildTool.Configuration;

public class EA : LuminaThirdPartyModuleRules
{
    public EA(TargetInfo Target)
        : base(Target)
    {
        // What dependents include: <EASTL/...> and the EABase common headers.
        PublicIncludePaths.Add("EASTL/include");
        PublicIncludePaths.Add("EABase/include/Common");

        PrivateIncludePaths.Add("EABase");
        PrivateIncludePaths.Add("EASTL");

        // Only EASTL ships compiled sources; EABase is headers.
        SourceDirectories.Add("EASTL/source");

        // Not merged, and for a different reason than the libraries that fail to compile: this one
        // links. A static library lets the linker take only the objects it needs, and EASTL relies
        // on that -- the engine defines eastl::AssertionFailure itself in EASTLImpl.cpp and expects
        // EASTL's own copy never to be pulled in. Merging puts that definition in the same object
        // as symbols the engine does reference, so it always arrives and collides (LNK2005).
        bUseUnityBuild = false;
    }
}
