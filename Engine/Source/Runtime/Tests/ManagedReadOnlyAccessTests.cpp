#include <gtest/gtest.h>

#include "Paths/Paths.h"
#include "Scripting/DotNet/DotNetHost.h"

using namespace Lumina;

namespace ManagedReadOnlyAccessTestsInternal
{
    using FErrorsFn = int32(*)(int32, char*, int32);
    using FCountFn  = int32(*)();
}

using namespace ManagedReadOnlyAccessTestsInternal;

class FManagedReadOnlyAccessTest : public ::testing::Test
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
            DotNet::Shutdown();
        }
    }

    void SetUp() override
    {
        if (!DotNet::IsInitialized())
        {
            GTEST_SKIP() << "the managed host did not start in this process";
        }
        Errors = (FErrorsFn)DotNet::ResolveManagedExport("Test_ReadOnlyAccessErrors");
        Count  = (FCountFn)DotNet::ResolveManagedExport("Test_ReadOnlyAccessCaseCount");
        ASSERT_NE(Errors, nullptr);
        ASSERT_NE(Count, nullptr);
    }

    FErrorsFn Errors = nullptr;
    FCountFn  Count  = nullptr;
};

TEST_F(FManagedReadOnlyAccessTest, ReadsCompileAndWritesThroughReadAccessDoNot)
{
    char First[1024] = {};
    EXPECT_EQ(Errors(0, First, sizeof(First)), 0) << "reads through Read<T> and TRes<T>, and writes through Write<T> and TResMut<T>, compile: " << First;

    const int32 Cases = Count();
    ASSERT_GT(Cases, 1);
    for (int32 Case = 1; Case < Cases; ++Case)
    {
        First[0] = 0;
        EXPECT_GT(Errors(Case, First, sizeof(First)), 0) << "write case " << Case << " compiled through read-only access";
    }
}
