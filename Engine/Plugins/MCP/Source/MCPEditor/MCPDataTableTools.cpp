#include "MCPDataTableTools.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentObjectEdit.h"
#include "Agent/AgentReflectionUtils.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Assets/AssetTypes/DataTable/DataTableCSV.h"
#include "Containers/Algorithm.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina::MCP
{
    namespace MCPDataTableToolsInternal
    {
        constexpr const char* GDataTableSaveHint = " Save with assets.save to persist.";

        struct FDataTableRowTarget
        {
            CDataTable* Table    = nullptr;
            CStruct*    RowType  = nullptr;
            int32       Index    = Constants::kIndexNone;
            void*       Memory   = nullptr;
        };

        bool ResolveDataTable(const FString& Guid, CDataTable*& OutTable, FString& OutError)
        {
            return Agent::ResolveAsset<CDataTable>(FStringView(Guid), OutTable, OutError);
        }

        // The first twenty names are enough for a model to spot a typo without flooding the reply.
        FString DataTableRowNameHint(const CDataTable* Table)
        {
            FString Names;
            for (int32 Index = 0; Index < Table->GetRowCount() && Index < 20; ++Index)
            {
                Names.Append(Index == 0 ? "" : ", ");
                Names.Append(Table->GetRowNameAt(Index).ToString());
            }
            return Names;
        }

        bool ResolveDataTableRow(const FString& Guid, const FString& RowName, FDataTableRowTarget& Out, FString& OutError)
        {
            if (!ResolveDataTable(Guid, Out.Table, OutError))
            {
                return false;
            }

            Out.RowType = Out.Table->GetRowStruct();
            if (Out.RowType == nullptr)
            {
                OutError = Lumina::Format("'{}' has no row struct yet; set RowStructName with assets.set_property.",
                    Out.Table->GetName());
                return false;
            }

            Out.Index = Out.Table->FindRowIndex(FName(RowName));
            if (Out.Index == Constants::kIndexNone)
            {
                OutError = Lumina::Format("No row named '{}'. Rows: {}.", RowName, DataTableRowNameHint(Out.Table));
                return false;
            }

            Out.Memory = Out.Table->Rows[Out.Index].Value.GetMutableMemory();
            return true;
        }

        void MarkDataTableDirty(CDataTable* Table)
        {
            if (Table->GetPackage() != nullptr)
            {
                Table->GetPackage()->MarkDirty();
            }
        }

        void RegisterDataTableDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDescribeDataTableParams, SDescribeDataTableResult>(
                Owner, "datatable.describe",
                "Report a data table's row struct, its columns and every row name.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SDescribeDataTableParams& In, SDescribeDataTableResult& Out)
                {
                    CDataTable* Table = nullptr;
                    FString Error;
                    if (!ResolveDataTable(In.Asset, Table, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    for (TObjectIterator<CStruct> It; It; ++It)
                    {
                        CStruct* Candidate = *It;
                        if (Candidate != SDataTableRowBase::StaticStruct()
                            && Candidate->IsChildOf(SDataTableRowBase::StaticStruct()))
                        {
                            Out.RowStructs.Emplace(*Candidate->GetName().ToString());
                        }
                    }

                    Out.RowCount = Table->GetRowCount();
                    for (int32 Index = 0; Index < Out.RowCount; ++Index)
                    {
                        Out.RowNames.Emplace(*Table->GetRowNameAt(Index).ToString());
                    }

                    CStruct* RowType = Table->GetRowStruct();
                    if (RowType == nullptr)
                    {
                        return Agent::FToolResult::Ok(Lumina::Format(
                            "'{}' has no row struct yet; set RowStructName with assets.set_property to one of the {} in RowStructs.",
                            Table->GetName(), Out.RowStructs.Num()));
                    }

                    Out.RowStruct = FString(*RowType->GetName().ToString());

                    TVector<FString> Unwritable;
                    Agent::CollectUnwritableFields(RowType, FStringView(), Unwritable);

                    for (CStruct* Current : Agent::Detail::CollectStructChain(RowType))
                    {
                        Current->ForEachProperty<FProperty>([&](FProperty* Property)
                        {
                            if (Property == nullptr)
                            {
                                return;
                            }

                            SDataTableColumnInfo Column;
                            Column.Name      = FString(*Property->GetPropertyName().ToString());
                            Column.Type      = FString(*Property->GetTypeName().ToString());
                            Column.bWritable = !Algo::Contains(Unwritable.begin(), Unwritable.end(), Column.Name);
                            Out.Columns.Add(Move(Column));
                        });
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("'{}' holds {} row(s) of {} with {} column(s).",
                        Table->GetName(), Out.RowCount, Out.RowStruct, Out.Columns.Num()));
                });
        }

        void RegisterDataTableGetRow(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SGetRowParams, SGetRowResult>(
                Owner, "datatable.get_row",
                "Read every field of one row as JSON.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SGetRowParams& In, SGetRowResult& Out)
                {
                    FDataTableRowTarget Target;
                    FString Error;
                    if (!ResolveDataTableRow(In.Asset, In.Row, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FJsonValue Value;
                    if (const Agent::FMarshalResult Written = Agent::WriteStruct(Target.RowType, Target.Memory, Value);
                        !Written.IsValid())
                    {
                        return Agent::FToolResult::Error(Written.Error);
                    }

                    Out.Row   = In.Row;
                    Out.Value = Value.ToString(2);
                    Agent::CollectUnwritableFields(Target.RowType, FStringView(), Out.UnwritableFields);

                    return Agent::FToolResult::Ok(Lumina::Format("{}\n{}", In.Row, Out.Value));
                });
        }

        void RegisterDataTableSetRowProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetRowPropertyParams, SRowPropertyResult>(
                Owner, "datatable.set_row_property",
                "Set one field on one row. Undoable only while the table is open in an editor.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetRowPropertyParams& In, SRowPropertyResult& Out)
                {
                    FDataTableRowTarget Target;
                    FString Error;
                    if (!ResolveDataTableRow(In.Asset, In.Row, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FJsonValue Value;
                    if (!FJsonValue::Parse(In.Value, Value))
                    {
                        return Agent::FToolResult::Error(
                            "Value is not JSON. Strings need quotes, so \"Torch\" rather than Torch.");
                    }

                    const Agent::FObjectEditResult Edit = Agent::SetObjectProperty(
                        Target.RowType, Target.Memory, Target.Table, FStringView(In.Path), Value, "Set Row Property (agent)");

                    if (!Edit.IsValid())
                    {
                        return Agent::FToolResult::Error(Edit.Error);
                    }

                    Out.Previous  = Edit.Previous;
                    Out.Current   = Edit.Current;
                    Out.bUndoable = Edit.bUndoable;

                    return Agent::FToolResult::Ok(Lumina::Format("{}.{} is now {}.{}{}",
                        In.Row, In.Path, Out.Current, GDataTableSaveHint,
                        Edit.bUndoable ? "" : " Applied without undo (no editor open for this table)."));
                });
        }

        Agent::FToolResult AddDataTableRow(const SDataTableRowParams& In, SDataTableRowResult& Out)
        {
            CDataTable* Table = nullptr;
            FString Error;
            if (!ResolveDataTable(In.Asset, Table, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            CStruct* RowType = Table->GetRowStruct();
            if (RowType == nullptr)
            {
                return Agent::FToolResult::Error(Lumina::Format(
                    "'{}' has no row struct yet; set RowStructName with assets.set_property.", Table->GetName()));
            }

            // Checked before the row exists, so a bad copy source leaves the table untouched.
            const int32 SourceIndex = In.CopyFrom.IsEmpty() ? Constants::kIndexNone : Table->FindRowIndex(FName(In.CopyFrom));
            if (!In.CopyFrom.IsEmpty() && SourceIndex == Constants::kIndexNone)
            {
                return Agent::FToolResult::Error(Lumina::Format("No row named '{}' to copy from. Rows: {}.",
                    In.CopyFrom, DataTableRowNameHint(Table)));
            }

            FJsonValue Value = FJsonValue::MakeObject();
            if (!In.Value.IsEmpty())
            {
                if (!FJsonValue::Parse(In.Value, Value) || !Value.IsObject())
                {
                    return Agent::FToolResult::Error("Value has to be a JSON object of row fields.");
                }
            }

            const FName Name = Table->MakeUniqueRowName(In.Name.IsEmpty() ? FName("NewRow") : FName(In.Name));
            Out.Index = Table->AddRow(Name);
            if (Out.Index == Constants::kIndexNone)
            {
                return Agent::FToolResult::Error(Lumina::Format(
                    "The row could not be added, since {} does not derive from SDataTableRowBase.", RowType->GetName().ToString()));
            }

            Out.Row = FString(*Name.ToString());
            void* Memory = Table->Rows[Out.Index].Value.GetMutableMemory();

            if (SourceIndex != Constants::kIndexNone)
            {
                RowType->CopyStruct(Memory, Table->Rows[SourceIndex].Value.GetMemory());
            }

            FString Applied;
            if (!Value.IsEmpty())
            {
                if (const Agent::FMarshalResult Read = Agent::ReadStruct(Value, RowType, Memory); !Read.IsValid())
                {
                    Applied = Lumina::Format(" Row values could not be applied: {}", Read.Error);
                }
            }

            MarkDataTableDirty(Table);

            return Agent::FToolResult::Ok(Lumina::Format("Added row '{}' at {}.{}{}", Out.Row, Out.Index, Applied, GDataTableSaveHint));
        }

        Agent::FToolResult RemoveDataTableRow(const SDataTableRowParams& In, SDataTableRowResult& Out)
        {
            FDataTableRowTarget Target;
            FString Error;
            if (!ResolveDataTableRow(In.Asset, In.Row, Target, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            Target.Table->RemoveRow(Target.Index);
            MarkDataTableDirty(Target.Table);

            Out.RemainingCount = Target.Table->GetRowCount();

            return Agent::FToolResult::Ok(Lumina::Format("Removed '{}'; {} row(s) remain.{}",
                In.Row, Out.RemainingCount, GDataTableSaveHint));
        }

        Agent::FToolResult RenameDataTableRow(const SDataTableRowParams& In, SDataTableRowResult& Out)
        {
            FDataTableRowTarget Target;
            FString Error;
            if (!ResolveDataTableRow(In.Asset, In.Row, Target, Error))
            {
                return Agent::FToolResult::Error(Error);
            }

            if (In.NewName.IsEmpty())
            {
                return Agent::FToolResult::Error("A row needs a name.");
            }

            if (Target.Table->HasRow(FName(In.NewName)))
            {
                return Agent::FToolResult::Error(Lumina::Format("A row named '{}' already exists.", In.NewName));
            }

            Target.Table->Rows[Target.Index].Name = FName(In.NewName);
            MarkDataTableDirty(Target.Table);

            Out.Row = In.NewName;
            return Agent::FToolResult::Ok(Lumina::Format("Renamed '{}' to '{}'.{}", In.Row, In.NewName, GDataTableSaveHint));
        }

        void RegisterDataTableRow(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDataTableRowParams, SDataTableRowResult>(
                Owner, "datatable.row",
                "Add, Remove or Rename a row. Add appends one, optionally copied from CopyFrom and with fields from Value. "
                "Rename refuses a name another row has.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SDataTableRowParams& In, SDataTableRowResult& Out)
                {
                    switch (In.Action)
                    {
                        case EDataTableRowAction::Remove: return RemoveDataTableRow(In, Out);
                        case EDataTableRowAction::Rename: return RenameDataTableRow(In, Out);
                        default:                          return AddDataTableRow(In, Out);
                    }
                });
        }

        Agent::FToolResult ImportDataTableCsv(CDataTable* Table, const SDataTableCsvParams& In, SDataTableCsvResult& Out)
        {
            const FDataTableCSVResult Imported = DataTableCSV::ImportText(Table, FStringView(In.Text));

            Out.RowCount       = Imported.RowsImported;
            Out.Skipped        = Imported.RowsSkipped;
            Out.Errors         = Imported.Errors;
            Out.UnknownColumns = Imported.UnknownColumns;

            if (!Imported.bSucceeded)
            {
                return Agent::FToolResult::Error(Imported.FailureReason.IsEmpty()
                    ? FString("The CSV could not be imported.") : Imported.FailureReason);
            }

            MarkDataTableDirty(Table);

            return Agent::FToolResult::Ok(Lumina::Format("Imported {} row(s), skipped {}, {} cell problem(s).{}",
                Out.RowCount, Out.Skipped, Out.Errors.Num(), GDataTableSaveHint));
        }

        void RegisterDataTableCsv(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDataTableCsvParams, SDataTableCsvResult>(
                Owner, "datatable.csv",
                "Export every row as CSV text, or Import CSV Text to replace every row. "
                "Import is all or nothing: a malformed file leaves the table as it was.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SDataTableCsvParams& In, SDataTableCsvResult& Out)
                {
                    CDataTable* Table = nullptr;
                    FString Error;
                    if (!ResolveDataTable(In.Asset, Table, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (In.Action == EDataTableCsvAction::Import)
                    {
                        return ImportDataTableCsv(Table, In, Out);
                    }

                    Out.Text = DataTableCSV::ExportText(Table);
                    return Agent::FToolResult::Ok(Out.Text);
                });
        }
    }

    using namespace MCPDataTableToolsInternal;

    void RegisterDataTableTools(FStringView Owner)
    {
        RegisterDataTableDescribe(Owner);
        RegisterDataTableGetRow(Owner);
        RegisterDataTableSetRowProperty(Owner);
        RegisterDataTableRow(Owner);
        RegisterDataTableCsv(Owner);
    }
}
