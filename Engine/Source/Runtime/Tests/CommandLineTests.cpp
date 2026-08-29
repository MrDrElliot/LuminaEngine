#include <gtest/gtest.h>

#include "Core/CommandLine/CommandLine.h"

using namespace Lumina;

namespace
{
    // Parse takes char*, matching what main hands it, so the fixtures cannot be string literals.
    class FArgs
    {
    public:

        explicit FArgs(std::initializer_list<const char*> InArgs)
        {
            Storage.reserve(InArgs.size());
            Pointers.reserve(InArgs.size());
            for (const char* Arg : InArgs)
            {
                Storage.push_back(FFixedString(Arg));
            }
            for (FFixedString& Arg : Storage)
            {
                Pointers.push_back(Arg.data());
            }
        }

        int   Count() const { return (int)Pointers.size(); }
        char** Values()     { return Pointers.data(); }

    private:

        TVector<FFixedString> Storage;
        TVector<char*>        Pointers;
    };
}

// Every lookup used to miss because the map keys on FName while find was handed a raw string.
TEST(CommandLine, AParsedArgumentIsFoundByName)
{
    FArgs Args({ "Lumina.exe", "--Project=H:/Projects/Game.lproject", "--map=/Game/Content/Maps/Test.lasset" });
    FCommandLine Parsed(Args.Count(), Args.Values());

    EXPECT_TRUE(Parsed.Has("project")) << "an argument that parsed must also be findable";
    EXPECT_TRUE(Parsed.Has("map"));

    ASSERT_TRUE(Parsed.Get("project").IsSet());
    EXPECT_EQ(Parsed.Get("project").value(), FFixedString("H:/Projects/Game.lproject"));

    ASSERT_TRUE(Parsed.Get("map").IsSet());
    EXPECT_EQ(Parsed.Get("map").value(), FFixedString("/Game/Content/Maps/Test.lasset"))
        << "the value's case and slashes must survive verbatim";
}

// The key is normalized on both sides, so the caller does not have to match how it was typed.
TEST(CommandLine, LookupIsCaseInsensitiveOnTheKeyOnly)
{
    FArgs Args({ "Lumina.exe", "--LogFile=Standalone" });
    FCommandLine Parsed(Args.Count(), Args.Values());

    EXPECT_TRUE(Parsed.Has("logfile"));
    EXPECT_TRUE(Parsed.Has("LOGFILE"));
    EXPECT_TRUE(Parsed.Has("LogFile"));

    ASSERT_TRUE(Parsed.Get("logfile").IsSet());
    EXPECT_EQ(Parsed.Get("logfile").value(), FFixedString("Standalone")) << "the value keeps its own case";
}

TEST(CommandLine, AnAbsentArgumentReportsAbsent)
{
    FArgs Args({ "Lumina.exe", "--map=/Game/A.lasset" });
    FCommandLine Parsed(Args.Count(), Args.Values());

    EXPECT_FALSE(Parsed.Has("project"));
    EXPECT_FALSE(Parsed.Get("project").IsSet());
    EXPECT_FALSE(Parsed.GetInt("port").IsSet());
}

// A flag with no value still has to register, which is how the dedicated server mode is selected.
TEST(CommandLine, AValuelessFlagIsPresentAndReadsAsTrue)
{
    FArgs Args({ "Lumina.exe", "--server", "--port=7778" });
    FCommandLine Parsed(Args.Count(), Args.Values());

    EXPECT_TRUE(Parsed.Has("server"));

    ASSERT_TRUE(Parsed.GetBool("server").IsSet());
    EXPECT_TRUE(Parsed.GetBool("server").value());

    ASSERT_TRUE(Parsed.GetInt("port").IsSet());
    EXPECT_EQ(Parsed.GetInt("port").value(), 7778);
}

// A separated value is the other spelling the parser accepts, and it must read back the same way.
TEST(CommandLine, ASeparatedValueParsesLikeAnEqualsValue)
{
    FArgs Args({ "Lumina.exe", "--map", "/Game/Content/Maps/Test.lasset" });
    FCommandLine Parsed(Args.Count(), Args.Values());

    ASSERT_TRUE(Parsed.Get("map").IsSet());
    EXPECT_EQ(Parsed.Get("map").value(), FFixedString("/Game/Content/Maps/Test.lasset"));
}
