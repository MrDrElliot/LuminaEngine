#include "Platform/GenericPlatform.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Scripting/DotNet/DotNetExport.h"

// Rows come back as raw pointers since the row type is only known at runtime; managed gates every read.

using namespace Lumina;

LUMINA_DOTNET_EXPORT(void*, DataTable_FindRow)(void* Table, const char* RowName, int32 Len)
{
    const CDataTable* DataTable = static_cast<const CDataTable*>(Table);
    if (DataTable == nullptr || RowName == nullptr || Len <= 0)
    {
        return nullptr;
    }
    return const_cast<void*>(DataTable->FindRow(FName(FStringView(RowName, (size_t)Len))));
}

LUMINA_DOTNET_EXPORT(void*, DataTable_GetRowAt)(void* Table, int32 Index)
{
    const CDataTable* DataTable = static_cast<const CDataTable*>(Table);
    if (DataTable == nullptr || Index < 0 || Index >= DataTable->GetRowCount())
    {
        return nullptr;
    }
    return const_cast<void*>(DataTable->Rows[Index].Value.GetMemory());
}

LUMINA_DOTNET_EXPORT(void*, DataTableHandle_GetRowMemory)(void* Handle)
{
    const SDataTableRowHandle* RowHandle = static_cast<const SDataTableRowHandle*>(Handle);
    return RowHandle ? const_cast<void*>(RowHandle->GetRowMemory()) : nullptr;
}
