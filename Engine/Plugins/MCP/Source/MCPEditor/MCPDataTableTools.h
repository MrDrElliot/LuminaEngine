#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPDataTableTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SDataTableColumnInfo
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        // Reflected type name, such as Float or Name or Map.
        PROPERTY()
        FString Type;

        // False for a field datatable.set_row_property cannot write.
        PROPERTY()
        bool bWritable = true;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeDataTableParams
    {
        GENERATED_BODY()

        // GUID or content path of the table.
        PROPERTY()
        FString Asset;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeDataTableResult
    {
        GENERATED_BODY()

        // Struct every row is an instance of; empty until one is picked.
        PROPERTY()
        FString RowStruct;

        PROPERTY()
        int32 RowCount = 0;

        PROPERTY()
        TVector<FString> RowNames;

        PROPERTY()
        TVector<SDataTableColumnInfo> Columns;

        // Every usable row struct, for assets.set_property on RowStructName.
        PROPERTY()
        TVector<FString> RowStructs;
    };

    REFLECT()
    struct MCPEDITOR_API SGetRowParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Asset;

        // Row name, from datatable.describe.
        PROPERTY()
        FString Row;
    };

    REFLECT()
    struct MCPEDITOR_API SGetRowResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Row;

        // Every field as JSON, in the shape datatable.set_row_property takes back.
        PROPERTY()
        FString Value;

        // Field paths that cannot be written.
        PROPERTY()
        TVector<FString> UnwritableFields;
    };

    REFLECT()
    struct MCPEDITOR_API SSetRowPropertyParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Asset;

        PROPERTY()
        FString Row;

        // Field path inside the row, such as MaxStackSize or Meshes[0].Socket.
        PROPERTY()
        FString Path;

        // The new value as JSON.
        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SRowPropertyResult
    {
        GENERATED_BODY()

        // Value before the change, as JSON.
        PROPERTY()
        FString Previous;

        // Value read back after the change, as JSON.
        PROPERTY()
        FString Current;

        // True when the table's open editor recorded the edit, so editor.history can undo it.
        PROPERTY()
        bool bUndoable = false;
    };

    REFLECT()
    enum class EDataTableRowAction : uint8
    {
        Add,
        Remove,
        Rename,
    };

    REFLECT()
    struct MCPEDITOR_API SDataTableRowParams
    {
        GENERATED_BODY()

        PROPERTY()
        EDataTableRowAction Action = EDataTableRowAction::Add;

        PROPERTY()
        FString Asset;

        // Remove and Rename only. Existing row name.
        PROPERTY()
        FString Row;

        // Add only. Wanted row name; empty or taken names get a numbered suffix.
        PROPERTY()
        FString Name;

        // Rename only.
        PROPERTY()
        FString NewName;

        // Add only. Existing row to copy fields from, applied before Value.
        PROPERTY()
        FString CopyFrom;

        // Add only. Fields to set on the new row, as a JSON object.
        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SDataTableRowResult
    {
        GENERATED_BODY()

        // Add and Rename. The name the row ended up with.
        PROPERTY()
        FString Row;

        // Add only.
        PROPERTY()
        int32 Index = 0;

        // Remove only. Rows left in the table.
        PROPERTY()
        int32 RemainingCount = 0;
    };

    REFLECT()
    enum class EDataTableCsvAction : uint8
    {
        Import,
        Export,
    };

    REFLECT()
    struct MCPEDITOR_API SDataTableCsvParams
    {
        GENERATED_BODY()

        PROPERTY()
        EDataTableCsvAction Action = EDataTableCsvAction::Export;

        PROPERTY()
        FString Asset;

        // Import only. CSV content, not a path; the header's first column is the row name.
        PROPERTY()
        FString Text;
    };

    REFLECT()
    struct MCPEDITOR_API SDataTableCsvResult
    {
        GENERATED_BODY()

        // Export only. CSV text in the shape Import takes back.
        PROPERTY()
        FString Text;

        // Import only. Rows imported.
        PROPERTY()
        int32 RowCount = 0;

        // Import only.
        PROPERTY()
        int32 Skipped = 0;

        // Import only. Per-cell problems the import worked around.
        PROPERTY()
        TVector<FString> Errors;

        // Import only. Header columns the row struct has no field for.
        PROPERTY()
        TVector<FString> UnknownColumns;
    };

    namespace MCP
    {
        void RegisterDataTableTools(FStringView Owner);
    }
}
