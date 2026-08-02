#pragma once

#include "Containers/Array.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CDataTable;

    /** What an import did, so the caller can report it instead of silently dropping rows. */
    struct FDataTableCSVResult
    {
        int32 RowsImported = 0;
        int32 RowsSkipped = 0;

        /** Columns present in the file that RowStruct has no property for. */
        TVector<FString> UnknownColumns;

        /** Properties on RowStruct the file never supplied; those rows keep their defaults. */
        TVector<FString> MissingColumns;

        /** Per-cell parse failures, "Row 'Goblin', column 'Health': ...". Capped so a wholly
         *  mismatched file reports a readable summary rather than one line per cell. */
        TVector<FString> Errors;

        bool bSucceeded = false;
        FString FailureReason;
    };

    namespace DataTableCSV
    {
        /** Replaces every row in Table from CSV text. The first line is the header; its first column
         *  is the row name regardless of what it is called, and the rest are matched to RowStruct
         *  properties by name. Table must already have a RowStruct.
         *
         *  All-or-nothing: on a hard failure (no RowStruct, empty file, no data rows) Table is left
         *  untouched. Individual bad cells are reported and skipped, not fatal. */
        RUNTIME_API FDataTableCSVResult ImportText(CDataTable* Table, FStringView Text);

        /** Round-trips ImportText. Columns are RowStruct's text-convertible properties in declared
         *  order; non-convertible properties are omitted since a CSV cell cannot represent them. */
        RUNTIME_API FString ExportText(const CDataTable* Table);
    }
}
