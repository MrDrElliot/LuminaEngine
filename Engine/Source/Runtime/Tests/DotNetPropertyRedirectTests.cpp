#include <gtest/gtest.h>
#include <cstring>

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/PropertyTypeTraits.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Scripting/ScriptableObject.h"
#include "ScriptTestUtil.h"

using namespace Lumina;

extern "C" void* LuminaSharp_FindClassByName(const char* Name, int Len);
extern "C" void LuminaSharp_StructAssign(void* Dst, const void* Src, const char* TypeName, int32 Len);

namespace
{
    struct FDotNetPropertyRedirectPayload
    {
        int32 Value = 0;
    };

    CStruct* RedirectPayloadStruct()
    {
        static constexpr FPropertyDesc Properties[] =
        {
            { .Name = "Value", .Offset = offsetof(FDotNetPropertyRedirectPayload, Value), .Type = PropertyTypeOf<int32> },
        };

        static CStruct* Struct = []
        {
            CStruct* New = NewObject<CStruct>(nullptr, "DotNetRedirectRenamedStruct_After");
            New->AddToRoot();
            New->Size = sizeof(FDotNetPropertyRedirectPayload);
            InitializeAndCreateFProperties(New, Properties, (uint32)std::size(Properties));
            New->Link();
            return New;
        }();
        return Struct;
    }
}

// The C# author renamed a class with [Alias]; FindClassByName must still resolve the old name the way every sibling lookup does.
TEST(DotNetPropertyRedirect, FindClassByNameFollowsARenameRedirect)
{
    Scripting::FScriptExportSchema Schema;
    CScriptClass* New = ScriptTest::MintWith("DotNetRedirectRenamedClass_After", Schema);
    ASSERT_NE(New, nullptr);
    FScriptableRegistry::RegisterClassRedirect(FName("DotNetRedirectRenamedClass_Before"), FName("DotNetRedirectRenamedClass_After"));

    const char* OldName = "DotNetRedirectRenamedClass_Before";
    EXPECT_EQ(LuminaSharp_FindClassByName(OldName, (int)strlen(OldName)), New);
}

// StructAssign is how a container element assignment lands; a renamed struct's old name must still copy through.
TEST(DotNetPropertyRedirect, StructAssignFollowsARenameRedirect)
{
    RedirectPayloadStruct();
    FScriptableRegistry::RegisterClassRedirect(FName("DotNetRedirectRenamedStruct_Before"), FName("DotNetRedirectRenamedStruct_After"));

    const FDotNetPropertyRedirectPayload Source{ .Value = 42 };
    FDotNetPropertyRedirectPayload Dest{};

    const char* OldName = "DotNetRedirectRenamedStruct_Before";
    LuminaSharp_StructAssign(&Dest, &Source, OldName, (int32)strlen(OldName));

    EXPECT_EQ(Dest.Value, 42);
}
