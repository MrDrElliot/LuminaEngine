#include "MCPDataTableTools.h"
#include "MCPToolTestUtil.h"

#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Scripting/InteropTestLibrary.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPDataTableToolsTestsInternal
{
    class MCPDataTableToolsTest : public FRegisteredToolsTest
    {
    protected:

        MCPDataTableToolsTest() : FRegisteredToolsTest("MCPDataTableToolsTests", &RegisterDataTableTools) {}

        void SetUp() override
        {
            RegisterPendingTypes();
            FRegisteredToolsTest::SetUp();
            const FString Package = Format("/MCPDataTableToolsTests/{}", testing::UnitTest::GetInstance()->current_test_info()->name());
            Table = NewObject<CDataTable>(CPackage::CreatePackage(Package), "Scratch");
            Table->SetRowStruct(SInteropTestDerivedRow::StaticStruct());
            Guid = FString(*Table->GetGUID().ToString());
        }

        const SInteropTestDerivedRow* Row(const char* Name) const
        {
            return FindDataTableRow<SInteropTestDerivedRow>(Table, FName(Name));
        }

        FJsonValue RowAction(FStringView Fields)
        {
            return ExpectSucceeded("datatable.row", FStringView(Format(R"({{"Asset":"{}",{}}})", Guid, Fields)));
        }

        FString ExportCsv()
        {
            const FJsonValue Result = ExpectSucceeded("datatable.csv", FStringView(Format(R"({{"Asset":"{}","Action":"Export"}})", Guid)));
            return FString(Result["structuredContent"]["Text"].AsString());
        }

        CDataTable* Table = nullptr;
        FString Guid;
    };
}

using namespace MCPDataTableToolsTestsInternal;

TEST_F(MCPDataTableToolsTest, AddCopiesFromAnotherRowThenAppliesValue)
{
    RowAction(R"("Name":"Sword","Value":{"BaseValue":3,"DerivedValue":4})");
    const FJsonValue Copy = RowAction(R"("Name":"Sword","CopyFrom":"Sword","Value":{"DerivedValue":9})");

    EXPECT_EQ(FString(Copy["structuredContent"]["Row"].AsString()), FString("Sword_1"));
    EXPECT_EQ(Copy["structuredContent"]["Index"].AsInt64(), 1);
    ASSERT_NE(Row("Sword_1"), nullptr);
    EXPECT_EQ(Row("Sword_1")->BaseValue, 3);
    EXPECT_EQ(Row("Sword_1")->DerivedValue, 9);
    EXPECT_EQ(Row("Sword")->DerivedValue, 4);
}

TEST_F(MCPDataTableToolsTest, AddCopiesFromAnEarlyRowAfterTheRowArrayGrows)
{
    RowAction(R"("Name":"Origin","Value":{"BaseValue":11,"DerivedValue":22})");

    // Past rpmalloc's large-class limit a relocation frees memory the allocator unmaps outright.
    Table->Rows.Reserve(65536);
    while (Table->Rows.Num() < Table->Rows.Max())
    {
        Table->AddRow(FName(Format("Filler{}", Table->Rows.Num())));
    }

    RowAction(R"("Name":"Copy","CopyFrom":"Origin")");

    const SInteropTestDerivedRow* Copied = Row("Copy");
    ASSERT_NE(Copied, nullptr);
    EXPECT_EQ(Copied->BaseValue, 11);
    EXPECT_EQ(Copied->DerivedValue, 22);
}

TEST_F(MCPDataTableToolsTest, AddRefusesAMissingCopySourceWithoutAddingARow)
{
    ExpectRefused("datatable.row", FStringView(Format(R"({{"Asset":"{}","Name":"Axe","CopyFrom":"Ghost"}})", Guid)), "No row named 'Ghost'");
    EXPECT_EQ(Table->GetRowCount(), 0);
}

TEST_F(MCPDataTableToolsTest, RenameRefusesATakenNameThenRemoveDropsTheRow)
{
    RowAction(R"("Name":"Sword")");
    RowAction(R"("Name":"Shield")");

    ExpectRefused("datatable.row", FStringView(Format(R"({{"Asset":"{}","Action":"Rename","Row":"Sword","NewName":"Shield"}})", Guid)), "already exists");
    RowAction(R"("Action":"Rename","Row":"Sword","NewName":"Blade")");
    EXPECT_FALSE(Table->HasRow(FName("Sword")));
    EXPECT_EQ(Table->FindRowIndex(FName("Blade")), 0);

    const FJsonValue Removed = RowAction(R"("Action":"Remove","Row":"Blade")");
    EXPECT_EQ(Removed["structuredContent"]["RemainingCount"].AsInt64(), 1);
    EXPECT_EQ(Table->GetRowNameAt(0), FName("Shield"));
}

TEST_F(MCPDataTableToolsTest, SetRowPropertyWritesAndGetRowReadsItBack)
{
    RowAction(R"("Name":"Sword")");

    const FJsonValue Set = ExpectSucceeded("datatable.set_row_property",
        FStringView(Format(R"({{"Asset":"{}","Row":"Sword","Path":"BaseValue","Value":12}})", Guid)));
    EXPECT_EQ(FString(Set["structuredContent"]["Previous"].AsString()), FString("0"));
    EXPECT_EQ(Row("Sword")->BaseValue, 12);

    const FJsonValue Got = ExpectSucceeded("datatable.get_row", FStringView(Format(R"({{"Asset":"{}","Row":"Sword"}})", Guid)));
    FJsonValue Value;
    ASSERT_TRUE(FJsonValue::Parse(FStringView(FString(Got["structuredContent"]["Value"].AsString())), Value));
    EXPECT_EQ(Value["BaseValue"].AsInt64(), 12);
}

TEST_F(MCPDataTableToolsTest, RowToolsRefuseARowWhoseStructNoLongerMatches)
{
    RowAction(R"("Name":"Stale")");
    Table->Rows[0].Value.InitializeAs(SInteropTestRow::StaticStruct());

    ExpectRefused("datatable.set_row_property",
        FStringView(Format(R"({{"Asset":"{}","Row":"Stale","Path":"BaseValue","Value":1}})", Guid)), "no longer matches");
    ExpectRefused("datatable.get_row", FStringView(Format(R"({{"Asset":"{}","Row":"Stale"}})", Guid)), "no longer matches");
    ExpectRefused("datatable.row", FStringView(Format(R"({{"Asset":"{}","CopyFrom":"Stale"}})", Guid)), "no longer matches");
    EXPECT_EQ(Table->GetRowCount(), 1);

    RowAction(R"("Action":"Remove","Row":"Stale")");
    EXPECT_EQ(Table->GetRowCount(), 0);
}

TEST_F(MCPDataTableToolsTest, CsvExportImportsBackUnchanged)
{
    const FJsonValue Imported = ExpectSucceeded("datatable.csv",
        FStringView(Format(R"({{"Asset":"{}","Action":"Import","Text":"Name,BaseValue,DerivedValue\nGoblin,5,6\nOrc,7,8\n"}})", Guid)));
    EXPECT_EQ(Imported["structuredContent"]["RowCount"].AsInt64(), 2);
    ASSERT_NE(Row("Orc"), nullptr);
    EXPECT_EQ(Row("Orc")->DerivedValue, 8);

    const FString Exported = ExportCsv();
    RowAction(R"("Action":"Remove","Row":"Goblin")");

    const FJsonValue Reimport = FJsonValue::MakeObject({ { "Asset", Guid }, { "Action", "Import" }, { "Text", Exported } });
    ExpectSucceeded("datatable.csv", FStringView(Reimport.ToString()));

    EXPECT_EQ(Table->GetRowCount(), 2);
    ASSERT_NE(Row("Goblin"), nullptr);
    EXPECT_EQ(Row("Goblin")->BaseValue, 5);
    EXPECT_EQ(ExportCsv(), Exported);
}
