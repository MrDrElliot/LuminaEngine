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
        // A wholly mismatched file would otherwise produce one error per cell, which is noise rather
        // than a report. The count still reflects reality; only the listing is capped.
        constexpr int32 GMaxReportedErrors = 32;

        /** One parsed record. Fields keep their order; a short row simply has fewer of them. */
        using FCSVRecord = TVector<FString>;

        /** RFC 4180 reader. Quoted fields may contain commas, CRLFs and "" escaped quotes; unquoted
         *  fields are taken verbatim up to the next comma or line break. */
        TVector<FCSVRecord> ParseRecords(FStringView Text)
        {
            TVector<FCSVRecord> Records;
            FCSVRecord Current;
            FString Field;
            bool bInQuotes = false;
            bool bFieldStarted = false;

            auto EndField = [&]()
            {
                Current.push_back(Field);
                Field.clear();
                bFieldStarted = false;
            };

            auto EndRecord = [&]()
            {
                EndField();

                // A line that is entirely empty is a blank separator, not a record of one empty field.
                const bool bBlank = Current.size() == 1 && Current[0].empty();
                if (!bBlank)
                {
                    Records.push_back(Current);
                }
                Current.clear();
            };

            const size_t Length = Text.size();
            for (size_t i = 0; i < Length; ++i)
            {
                const char C = Text[i];

                if (bInQuotes)
                {
                    if (C == '"')
                    {
                        // "" inside a quoted field is a literal quote; a lone " closes the field.
                        if (i + 1 < Length && Text[i + 1] == '"')
                        {
                            Field.push_back('"');
                            ++i;
                        }
                        else
                        {
                            bInQuotes = false;
                        }
                    }
                    else
                    {
                        Field.push_back(C);
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
                        Field.push_back(C);
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
                    Field.push_back(C);
                    bFieldStarted = true;
                    break;
                }
            }

            // Trailing content with no final newline is still a record.
            if (!Field.empty() || !Current.empty())
            {
                EndRecord();
            }

            return Records;
        }

        FString Trim(const FString& In)
        {
            size_t Begin = 0;
            size_t End = In.size();
            while (Begin < End && (In[Begin] == ' ' || In[Begin] == '\t')) { ++Begin; }
            while (End > Begin && (In[End - 1] == ' ' || In[End - 1] == '\t')) { --End; }
            return FString(In.data() + Begin, End - Begin);
        }

        /** Quotes only when it has to: a file of plain values stays diffable. */
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
            Out.reserve(In.size() + 2);
            Out.push_back('"');
            for (const char C : In)
            {
                if (C == '"')
                {
                    Out.push_back('"');
                }
                Out.push_back(C);
            }
            Out.push_back('"');
            return Out;
        }

        /** Text-convertible properties of Struct, supers first, in declared order. Matches the order
         *  ExportText writes and the grid displays. */
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
                    Out.push_back(Property);
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

        const TVector<FCSVRecord> Records = ParseRecords(Text);
        if (Records.empty())
        {
            Result.FailureReason = "The file is empty.";
            return Result;
        }
        if (Records.size() < 2)
        {
            Result.FailureReason = "The file has a header but no data rows.";
            return Result;
        }

        // Resolve the header once. Column 0 is the row name whatever it is called, since a name is
        // structural here rather than a field on the row struct.
        const FCSVRecord& Header = Records[0];
        TVector<FProperty*> ColumnProperties;
        ColumnProperties.reserve(Header.size());
        ColumnProperties.push_back(nullptr);

        THashSet<FName> MatchedProperties;
        for (size_t Col = 1; Col < Header.size(); ++Col)
        {
            const FString ColumnName = Trim(Header[Col]);
            FProperty* Property = ColumnName.empty() ? nullptr : RowStruct->GetProperty(FName(ColumnName));

            if (Property != nullptr && !Reflection::IsTextConvertible(Property))
            {
                // Named a real field, but one a CSV cell cannot represent. Report it as unknown
                // rather than pretending it imported.
                Property = nullptr;
            }

            if (Property == nullptr)
            {
                if (!ColumnName.empty())
                {
                    Result.UnknownColumns.push_back(ColumnName);
                }
            }
            else
            {
                MatchedProperties.insert(Property->GetPropertyName());
            }

            ColumnProperties.push_back(Property);
        }

        TVector<FProperty*> AllColumns;
        GatherColumns(RowStruct, AllColumns);
        for (FProperty* Property : AllColumns)
        {
            if (MatchedProperties.find(Property->GetPropertyName()) == MatchedProperties.end())
            {
                Result.MissingColumns.push_back(Property->GetPropertyName().ToString());
            }
        }

        // Build into a staging table first: a parse that dies partway must not leave the asset holding
        // half of one file and half of another.
        TVector<SDataTableRow> NewRows;
        NewRows.reserve(Records.size() - 1);
        THashSet<FName> SeenNames;

        for (size_t RecordIndex = 1; RecordIndex < Records.size(); ++RecordIndex)
        {
            const FCSVRecord& Record = Records[RecordIndex];

            const FString RawName = Record.empty() ? FString() : Trim(Record[0]);
            if (RawName.empty())
            {
                ++Result.RowsSkipped;
                if ((int32)Result.Errors.size() < GMaxReportedErrors)
                {
                    Result.Errors.push_back(Format("Line {}: the row name is empty.", (int32)RecordIndex + 1));
                }
                continue;
            }

            const FName RowName(RawName);
            if (SeenNames.find(RowName) != SeenNames.end())
            {
                // Skipped rather than overwritten: a duplicate key means the file is wrong, and
                // silently keeping the last one hides which value won.
                ++Result.RowsSkipped;
                if ((int32)Result.Errors.size() < GMaxReportedErrors)
                {
                    Result.Errors.push_back(Format("Line {}: duplicate row name '{}'.", (int32)RecordIndex + 1, RawName));
                }
                continue;
            }
            SeenNames.insert(RowName);

            SDataTableRow& Row = NewRows.emplace_back();
            Row.Name = RowName;
            Row.Value.InitializeAs(RowStruct);

            void* RowMemory = Row.Value.GetMutableMemory();

            // Short rows are legal: the absent columns keep the struct's defaults.
            const size_t CellCount = Math::Min(Record.size(), ColumnProperties.size());
            for (size_t Col = 1; Col < CellCount; ++Col)
            {
                FProperty* Property = ColumnProperties[Col];
                if (Property == nullptr)
                {
                    continue;
                }

                const FString Cell = Trim(Record[Col]);
                if (!Reflection::FromText(Property, RowMemory, Cell))
                {
                    if ((int32)Result.Errors.size() < GMaxReportedErrors)
                    {
                        Result.Errors.push_back(Format("Line {}, column '{}': cannot read '{}'.",
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
