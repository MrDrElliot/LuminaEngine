#include <gtest/gtest.h>

#include "Containers/StringFormat.h"
#include "GUID/GUID.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Scripting/DotNet/ScriptReferences.h"

using namespace Lumina;

TEST(ScriptReferencesTests, AMissingLocalAssemblyIsNotCachedAndIsPickedUpOnceItAppears)
{
    const FString UnitName = Format("MissingAssemblyUnit_{}", FGuid::New());
    const FString AssemblyPath = Format("{}/LuminaScriptReferencesTest_{}.dll", Filesystem::GetTempDirectory(), FGuid::New());

    DotNet::FScriptReferenceSet Declared;
    Declared.Assemblies.Add(AssemblyPath);

    TVector<FString> FirstPass;
    ASSERT_TRUE(DotNet::ResolveScriptReferences(UnitName, FStringView(), Declared, FirstPass));
    EXPECT_TRUE(FirstPass.IsEmpty());

    ASSERT_TRUE(FileHelper::SaveStringToFile("not a real assembly, only needs to exist", AssemblyPath));

    TVector<FString> SecondPass;
    const bool bResolved = DotNet::ResolveScriptReferences(UnitName, FStringView(), Declared, SecondPass);
    Filesystem::RemoveFile(AssemblyPath);

    ASSERT_TRUE(bResolved);
    ASSERT_EQ(SecondPass.Num(), 1);
    EXPECT_EQ(SecondPass[0], AssemblyPath);
}
