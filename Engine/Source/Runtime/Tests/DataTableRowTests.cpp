#include <gtest/gtest.h>

#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Core/Object/ObjectCore.h"
#include "GUID/GUID.h"
#include "Scripting/InteropTestLibrary.h"

using namespace Lumina;

namespace
{
    TStrongObjectPtr<CDataTable> NewTable(CStruct* RowStruct, const char* RowName)
    {
        TStrongObjectPtr<CDataTable> Table(NewObject<CDataTable>(nullptr, NAME_None, FGuid::New(), OF_Transient));
        Table->SetRowStruct(RowStruct);
        Table->AddRow(FName(RowName));
        return Table;
    }

    SDataTableRowHandle MakeHandle(const TStrongObjectPtr<CDataTable>& Table, const char* RowName)
    {
        SDataTableRowHandle Handle;
        Handle.DataTable = Table;
        Handle.RowName = FName(RowName);
        return Handle;
    }
}

TEST(DataTableRows, DerivedRowsReadAsTheirBase)
{
    const TStrongObjectPtr<CDataTable> Table = NewTable(SInteropTestDerivedRow::StaticStruct(), "Sword");
    const SDataTableRowHandle Handle = MakeHandle(Table, "Sword");

    const SInteropTestDerivedRow* AsDerived = FindDataTableRow<SInteropTestDerivedRow>(Handle);
    const SInteropTestRow* AsBase = FindDataTableRow<SInteropTestRow>(Handle);
    ASSERT_NE(AsDerived, nullptr);
    ASSERT_NE(AsBase, nullptr);

    // A field read back through the base proves the base view starts where the row does.
    const_cast<SInteropTestDerivedRow*>(AsDerived)->BaseValue = 7;
    EXPECT_EQ(AsBase->BaseValue, 7);
    EXPECT_EQ(FindDataTableRow<SInteropTestRow>(Table.Get(), FName("Sword")), AsBase);
    EXPECT_TRUE(Table->RowsAre(SInteropTestRow::StaticStruct()));
    EXPECT_TRUE(Table->RowsAreNamed(StructIdentity(SInteropTestRow::StaticStruct())));
}

TEST(DataTableRows, BaseRowsDoNotReadAsDerived)
{
    const TStrongObjectPtr<CDataTable> Table = NewTable(SInteropTestRow::StaticStruct(), "Rock");
    const SDataTableRowHandle Handle = MakeHandle(Table, "Rock");

    EXPECT_NE(FindDataTableRow<SInteropTestRow>(Handle), nullptr);
    EXPECT_EQ(FindDataTableRow<SInteropTestDerivedRow>(Handle), nullptr);
    EXPECT_EQ(FindDataTableRow<SInteropTestDerivedRow>(Table.Get(), FName("Rock")), nullptr);
    EXPECT_FALSE(Table->RowsAre(SInteropTestDerivedRow::StaticStruct()));
}

TEST(DataTableRows, EmptyOrMissingHandlesReadNothing)
{
    const TStrongObjectPtr<CDataTable> Table = NewTable(SInteropTestRow::StaticStruct(), "Rock");

    EXPECT_EQ(FindDataTableRow<SInteropTestRow>(SDataTableRowHandle()), nullptr);
    EXPECT_EQ(FindDataTableRow<SInteropTestRow>(MakeHandle(Table, "Missing")), nullptr);
    EXPECT_EQ(FindDataTableRow<SInteropTestRow>(static_cast<const CDataTable*>(nullptr), FName("Rock")), nullptr);
    EXPECT_FALSE(Table->RowsAre(nullptr));
    EXPECT_FALSE(Table->RowsAreNamed(FName()));
}
