#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "DataTableFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CDataTableFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Data Table"; }
        FStringView GetDefaultAssetCreationName() override { return "NewDataTable"; }
        FString GetAssetDescription() const override { return "A table of named rows, each an instance of a chosen row struct."; }
        CClass* GetAssetClass() const override { return CDataTable::StaticClass(); }
        FString GetCategory() const override { return "Data"; }

        // Required, since choosing the row struct later would discard whatever had been authored.
        bool HasCreationDialogue() const override;
        bool DrawCreationDialogue(FStringView Path, bool& bShouldClose) override;

    private:

        CStruct* SelectedRowStruct = nullptr;
    };
}
