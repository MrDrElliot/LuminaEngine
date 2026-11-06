#include "RuntimePCH.h"
#include "DataTableCSV.h"

#include "DataTable.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/PropertyText.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina::DataTableCSV
{
    namespace
    {
        // The count still reflects reality, and only the listing is capped.
        constexpr int32 GMaxReportedErrors = 32;

        /** One parsed record. Fields keep their order; a short row simply has fewer of them. */
        using FCSVRecord = TVector<FString>;

        // Quoted fields may hold commas, line breaks and escaped quotes, and unquoted ones are verbatim.
        TVector<FCSVRecord> ParseRecords(FStringView Text)
        {
            TVector<FCSVRecord> Records;
            FCSVRecord Current;
            FString Field;
            bool bInQuotes = false;
            bool bFieldStarted = false;

            auto EndField = [&]()
            {
                Current.Add(Field);
                Field.Reset();
                bFieldStarted = false;
            };

            auto EndRecord = [&]()
            {
                EndField();

                // A line that is entirely empty is a blank separator, not a record of one empty field.
                const bool bBlank = Current.Num() == 1 && Current[0].IsEmpty();
                if (!bBlank)
                {
                    Records.Add(Current);
                }
                Current.Reset();
            };

            const int32 Length = Text.Len();
            for (int32 i = 0; i < Length; ++i)
            {
                const char C = Text[i];

                if (bInQuotes)
                {
                    if (C == '"')
                    {
                        // "" inside a quoted field is a literal quote; a lone " closes the field.
                        if (i + 1 < Length && Text[i + 1] == '"')
                        {
                            Field.AppendChar('"');
                            ++i;
                        }
                        else
                        {
                            bInQuotes = false;
                        }
                    }
                    else
                    {
                        Field.AppendChar(C);
                    }
                    continue;
                }

                switch (C)
                {
                case '"':
                    // Only opens a quoted field at the start; a stray quote mid-field is literal text.
                    if (!bFieldStarted)
                    {
                        bInQuotes = true;
                        bFieldStarted = true;
                    }
                    else
                    {
                        Field.AppendChar(C);
                    }
                    break;

                case ',':
                    EndField();
                    break;

                case '\r':
                    // Swallow the LF of a CRLF so the pair ends exactly one record.
                    if (i + 1 < Length && Text[i + 1] == '\n')
                    {
                        ++i;
                    }
                    EndRecord();
                    break;

                case '\n':
                    EndRecord();
                    break;

                default:
                    Field.AppendChar(C);
                    bFieldStarted = true;
                    break;
                }
            }

            // Trailing content with no final newline is still a record.
            if (!Field.IsEmpty() || !Current.IsEmpty())
            {
                EndRecord();
            }

            return Records;
        }

        FString Trim(const FString& In)
        {
            int32 Begin = 0;
            int32 End = In.Len();
            while (Begin < End && (In[Begin] == ' ' || In[Begin] == '\t')) { ++Begin; }
            while (End > Begin && (In[End - 1] == ' ' || In[End - 1] == '\t')) { --End; }
            return FString(In.GetData() + Begin, End - Begin);
        }

        // Quotes only when it has to, so a file of plain values stays diffable.
        FString Escape(const FString& In)
        {
            bool bNeedsQuotes = false;
            for (const char C : In)
            {
                if (C == ',' || C == '"' || C == '\n' || C == '\r')
                {
                    bNeedsQuotes = true;
                    break;
                }
            }

            if (!bNeedsQuotes)
            {
                return In;
            }

            FString Out;
            Out.Reserve(In.Len() + 2);
            Out.AppendChar('"');
            for (const char C : In)
            {
                if (C == '"')
                {
                    Out.AppendChar('"');
                }
                Out.AppendChar(C);
            }
            Out.AppendChar('"');
            return Out;
        }

        // Matches the order ExportText writes and the grid displays.
        void GatherColumns(CStruct* Struct, TVector<FProperty*>& Out)
        {
            if (Struct == nullptr)
            {
                return;
            }

            GatherColumns(Struct->GetSuperStruct(), Out);

            Struct->ForEachProperty<FProperty>([&Out](FProperty* Property)
            {
                if (Reflection::IsTextConvertible(Property))
                {
                    Out.Add(Property);
                }
            });
        }
    }

    FDataTableCSVResult ImportText(CDataTable* Table, FStringView Text)
    {
        FDataTableCSVResult Result;

        CStruct* RowStruct = Table != nullptr ? Table->GetRowStruct() : nullptr;
        if (RowStruct == nullptr)
        {
            Result.FailureReason = "The table has no row struct set.";
            return Result;
        }
        if (!RowStruct->IsChildOf(SDataTableRowBase::StaticStruct()))
        {
            Result.FailureReason = Format("The row struct {} does not derive from SDataTableRowBase.", RowStruct->GetName().ToString());
            return Result;
        }

        const TVector<FCSVRecord> Records = ParseRecords(Text);
        if (Records.IsEmpty())
        {
            Result.FailureReason = "The file is empty.";
            return Result;
        }
        if (Records.Num() < 2)
        {
            Result.FailureReason = "The file has a header but no data rows.";
            return Result;
        }

        // Column 0 is the row name whatever it is called, since a name is structural rather than a field.
        const FCSVRecord& Header = Records[0];
        TVector<FProperty*> ColumnProperties;
        ColumnProperties.Reserve(Header.Num());
        ColumnProperties.Add(nullptr);

        THashSet<FName> MatchedProperties;
        for (int32 Col = 1; Col < Header.Num(); ++Col)
        {
            const FString ColumnName = Trim(Header[Col]);
            FProperty* Property = ColumnName.IsEmpty() ? nullptr : RowStruct->GetProperty(FName(ColumnName));

            if (Property != nullptr && !Reflection::IsTextConvertible(Property))
            {
                // It named a real field that a CSV cell cannot represent, so report unknown rather than imported.
                Property = nullptr;
            }

            if (Property == nullptr)
            {
                if (!ColumnName.IsEmpty())
                {
                    Result.UnknownColumns.Add(ColumnName);
                }
            }
            else
            {
                MatchedProperties.Add(Property->GetPropertyName());
            }

            ColumnProperties.Add(Property);
        }

        TVector<FProperty*> AllColumns;
        GatherColumns(RowStruct, AllColumns);
        for (FProperty* Property : AllColumns)
        {
            if (!MatchedProperties.Contains(Property->GetPropertyName()))
            {
                Result.MissingColumns.Add(Property->GetPropertyName().ToString());
            }
        }

        // A parse that dies partway must not leave the asset holding half of two files.
        TVector<SDataTableRow> NewRows;
        NewRows.Reserve(Records.Num() - 1);
        THashSet<FName> SeenNames;

        for (int32 RecordIndex = 1; RecordIndex < Records.Num(); ++RecordIndex)
        {
            const FCSVRecord& Record = Records[RecordIndex];

            const FString RawName = Record.IsEmpty() ? FString() : Trim(Record[0]);
            if (RawName.IsEmpty())
            {
                ++Result.RowsSkipped;
                if (Result.Errors.Num() < GMaxReportedErrors)
                {
                    Result.Errors.Add(Format("Line {}: the row name is empty.", (int32)RecordIndex + 1));
                }
                continue;
            }

            const FName RowName(RawName);
            if (SeenNames.Contains(RowName))
            {
                // A duplicate key means the file is wrong, and keeping the last one hides which value won.
                ++Result.RowsSkipped;
                if (Result.Errors.Num() < GMaxReportedErrors)
                {
                    Result.Errors.Add(Format("Line {}: duplicate row name '{}'.", (int32)RecordIndex + 1, RawName));
                }
                continue;
            }
            SeenNames.Add(RowName);

            SDataTableRow& Row = NewRows.EmplaceGetRef();
            Row.Name = RowName;
            Row.Value.InitializeAs(RowStruct);

            void* RowMemory = Row.Value.GetMutableMemory();

            // Short rows are legal, and the absent columns keep the struct's defaults.
            const int32 CellCount = Math::Min(Record.Num(), ColumnProperties.Num());
            for (int32 Col = 1; Col < CellCount; ++Col)
            {
                FProperty* Property = ColumnProperties[Col];
                if (Property == nullptr)
                {
                    continue;
                }

                const FString Cell = Trim(Record[Col]);
                if (!Reflection::FromText(Property, RowMemory, Cell))
                {
                    if (Result.Errors.Num() < GMaxReportedErrors)
                    {
                        Result.Errors.Add(Format("Line {}, column '{}': cannot read '{}'.",
                            (int32)RecordIndex + 1,
                            Property->GetPropertyName().ToString(),
                            Cell));
                    }
                }
            }

            ++Result.RowsImported;
        }

        if (Result.RowsImported == 0)
        {
            Result.FailureReason = "No rows could be read from the file.";
            return Result;
        }

        Table->Rows = std::move(NewRows);
        Result.bSucceeded = true;
        return Result;
    }

    FString ExportText(const CDataTable* Table)
    {
        CStruct* RowStruct = Table != nullptr ? Table->GetRowStruct() : nullptr;
        if (RowStruct == nullptr)
        {
            return FString();
        }

        TVector<FProperty*> Columns;
        GatherColumns(RowStruct, Columns);

        FString Out;
        Out += "Name";
        for (FProperty* Property : Columns)
        {
            Out += ",";
            Out += Escape(Property->GetPropertyName().ToString());
        }
        Out += "\n";

        for (const SDataTableRow& Row : Table->Rows)
        {
            Out += Escape(Row.Name.ToString());

            const void* RowMemory = Row.Value.GetMemory();
            for (FProperty* Property : Columns)
            {
                Out += ",";
                if (RowMemory != nullptr)
                {
                    Out += Escape(Reflection::ToText(Property, RowMemory));
                }
            }
            Out += "\n";
        }

        return Out;
    }
}
