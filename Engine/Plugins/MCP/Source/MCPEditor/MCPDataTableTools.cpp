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
    namespace
    {
        constexpr const char* GDataTableSaveHint = " Save with assets.save to persist.";

        struct FRowTarget
        {
            CDataTable* Table    = nullptr;
            CStruct*    RowType  = nullptr;
            int32       Index    = Constants::kIndexNone;
            void*       Memory   = nullptr;
        };

        bool ResolveTable(const FString& Guid, CDataTable*& OutTable, FString& OutError)
        {
            return Agent::ResolveAsset<CDataTable>(FStringView(Guid), OutTable, OutError);
        }

        // The first twenty names are enough for a model to spot a typo without flooding the reply.
        FString RowNameHint(const CDataTable* Table)
        {
            FString Names;
            for (int32 Index = 0; Index < Table->GetRowCount() && Index < 20; ++Index)
            {
                Names.append(Index == 0 ? "" : ", ");
                Names.append(Table->GetRowNameAt(Index).ToString());
            }
            return Names;
        }

        bool ResolveRow(const FString& Guid, const FString& RowName, FRowTarget& Out, FString& OutError)
        {
            if (!ResolveTable(Guid, Out.Table, OutError))
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
                OutError = Lumina::Format("No row named '{}'. Rows: {}.", RowName, RowNameHint(Out.Table));
                return false;
            }

            Out.Memory = Out.Table->Rows[Out.Index].Value.GetMutableMemory();
            return true;
        }

        void MarkDirty(CDataTable* Table)
        {
            if (Table->GetPackage() != nullptr)
            {
                Table->GetPackage()->MarkDirty();
            }
        }

        void RegisterDescribeDataTable(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDescribeDataTableParams, SDescribeDataTableResult>(
                Owner, "datatable.describe",
                "Report a data table's row struct, its columns and every row name.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SDescribeDataTableParams& In, SDescribeDataTableResult& Out)
                {
                    CDataTable* Table = nullptr;
                    FString Error;
                    if (!ResolveTable(In.Asset, Table, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    for (TObjectIterator<CStruct> It; It; ++It)
                    {
                        CStruct* Candidate = *It;
                        if (Candidate != SDataTableRowBase::StaticStruct()
                            && Candidate->IsChildOf(SDataTableRowBase::StaticStruct()))
                        {
                            Out.RowStructs.push_back(FString(Candidate->GetName().ToString().c_str()));
                        }
                    }

                    Out.RowCount = Table->GetRowCount();
                    for (int32 Index = 0; Index < Out.RowCount; ++Index)
                    {
                        Out.RowNames.push_back(FString(Table->GetRowNameAt(Index).ToString().c_str()));
                    }

                    CStruct* RowType = Table->GetRowStruct();
                    if (RowType == nullptr)
                    {
                        return Agent::FToolResult::Ok(Lumina::Format(
                            "'{}' has no row struct yet; set RowStructName with assets.set_property to one of the {} in RowStructs.",
                            Table->GetName(), Out.RowStructs.size()));
                    }

                    Out.RowStruct = FString(RowType->GetName().ToString().c_str());

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
                            Column.Name      = FString(Property->GetPropertyName().ToString().c_str());
                            Column.Type      = FString(Property->GetTypeName().ToString().c_str());
                            Column.bWritable = !Algo::Contains(Unwritable.begin(), Unwritable.end(), Column.Name);
                            Out.Columns.push_back(Move(Column));
                        });
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("'{}' holds {} row(s) of {} with {} column(s).",
                        Table->GetName(), Out.RowCount, Out.RowStruct, Out.Columns.size()));
                });
        }

        void RegisterGetRow(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SGetRowParams, SGetRowResult>(
                Owner, "datatable.get_row",
                "Read every field of one row as JSON.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SGetRowParams& In, SGetRowResult& Out)
                {
                    FRowTarget Target;
                    FString Error;
                    if (!ResolveRow(In.Asset, In.Row, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    nlohmann::json Value;
                    if (const Agent::FMarshalResult Written = Agent::WriteStruct(Target.RowType, Target.Memory, Value);
                        !Written.IsValid())
                    {
                        return Agent::FToolResult::Error(Written.Error);
                    }

                    Out.Row   = In.Row;
                    Out.Value = FString(Value.dump(2).c_str());
                    Agent::CollectUnwritableFields(Target.RowType, FStringView(), Out.UnwritableFields);

                    return Agent::FToolResult::Ok(Lumina::Format("{}\n{}", In.Row, Out.Value));
                });
        }

        void RegisterSetRowProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetRowPropertyParams, SRowPropertyResult>(
                Owner, "datatable.set_row_property",
                "Set one field on one row. Undoable only while the table is open in an editor.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetRowPropertyParams& In, SRowPropertyResult& Out)
                {
                    FRowTarget Target;
                    FString Error;
                    if (!ResolveRow(In.Asset, In.Row, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    nlohmann::json Value = nlohmann::json::parse(In.Value.c_str(), nullptr, false);
                    if (Value.is_discarded())
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

        void RegisterAddRow(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddRowParams, SAddRowResult>(
                Owner, "datatable.add_row",
                "Append a row, optionally copied from another row and with fields set from JSON.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddRowParams& In, SAddRowResult& Out)
                {
                    CDataTable* Table = nullptr;
                    FString Error;
                    if (!ResolveTable(In.Asset, Table, Error))
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
                    const int32 SourceIndex = In.CopyFrom.empty() ? Constants::kIndexNone : Table->FindRowIndex(FName(In.CopyFrom));
                    if (!In.CopyFrom.empty() && SourceIndex == Constants::kIndexNone)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No row named '{}' to copy from. Rows: {}.",
                            In.CopyFrom, RowNameHint(Table)));
                    }

                    nlohmann::json Value = nlohmann::json::object();
                    if (!In.Value.empty())
                    {
                        Value = nlohmann::json::parse(In.Value.c_str(), nullptr, false);
                        if (Value.is_discarded() || !Value.is_object())
                        {
                            return Agent::FToolResult::Error("Value has to be a JSON object of row fields.");
                        }
                    }

                    const FName Name = Table->MakeUniqueRowName(In.Name.empty() ? FName("NewRow") : FName(In.Name));
                    Out.Index = Table->AddRow(Name);
                    if (Out.Index == Constants::kIndexNone)
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "The row could not be added, since {} does not derive from SDataTableRowBase.", RowType->GetName().ToString()));
                    }

                    Out.Row = FString(Name.ToString().c_str());
                    void* Memory = Table->Rows[Out.Index].Value.GetMutableMemory();

                    if (SourceIndex != Constants::kIndexNone)
                    {
                        RowType->CopyStruct(Memory, Table->Rows[SourceIndex].Value.GetMemory());
                    }

                    FString Applied;
                    if (!Value.empty())
                    {
                        if (const Agent::FMarshalResult Read = Agent::ReadStruct(Value, RowType, Memory); !Read.IsValid())
                        {
                            Applied = Lumina::Format(" Row values could not be applied: {}", Read.Error);
                        }
                    }

                    MarkDirty(Table);

                    return Agent::FToolResult::Ok(Lumina::Format("Added row '{}' at {}.{}{}", Out.Row, Out.Index, Applied, GDataTableSaveHint));
                });
        }

        void RegisterRemoveRow(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SRemoveRowParams, SRemoveRowResult>(
                Owner, "datatable.remove_row",
                "Delete one row by name.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SRemoveRowParams& In, SRemoveRowResult& Out)
                {
                    FRowTarget Target;
                    FString Error;
                    if (!ResolveRow(In.Asset, In.Row, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Target.Table->RemoveRow(Target.Index);
                    MarkDirty(Target.Table);

                    Out.bRemoved       = true;
                    Out.RemainingCount = Target.Table->GetRowCount();

                    return Agent::FToolResult::Ok(Lumina::Format("Removed '{}'; {} row(s) remain.{}",
                        In.Row, Out.RemainingCount, GDataTableSaveHint));
                });
        }

        void RegisterRenameRow(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SRenameRowParams, SRenameRowResult>(
                Owner, "datatable.rename_row",
                "Rename one row. Refuses a name another row already has.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SRenameRowParams& In, SRenameRowResult& Out)
                {
                    FRowTarget Target;
                    FString Error;
                    if (!ResolveRow(In.Asset, In.Row, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (In.NewName.empty())
                    {
                        return Agent::FToolResult::Error("A row needs a name.");
                    }

                    if (Target.Table->HasRow(FName(In.NewName)))
                    {
                        return Agent::FToolResult::Error(Lumina::Format("A row named '{}' already exists.", In.NewName));
                    }

                    Target.Table->Rows[Target.Index].Name = FName(In.NewName);
                    MarkDirty(Target.Table);

                    Out.Row = In.NewName;
                    return Agent::FToolResult::Ok(Lumina::Format("Renamed '{}' to '{}'.{}", In.Row, In.NewName, GDataTableSaveHint));
                });
        }

        void RegisterImportCsv(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SImportCsvParams, SImportCsvResult>(
                Owner, "datatable.import_csv",
                "Replace every row from CSV text. All or nothing: a malformed file leaves the table as it was.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SImportCsvParams& In, SImportCsvResult& Out)
                {
                    CDataTable* Table = nullptr;
                    FString Error;
                    if (!ResolveTable(In.Asset, Table, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    const FDataTableCSVResult Imported = DataTableCSV::ImportText(Table, FStringView(In.Text));

                    Out.RowCount       = Imported.RowsImported;
                    Out.Skipped        = Imported.RowsSkipped;
                    Out.Errors         = Imported.Errors;
                    Out.UnknownColumns = Imported.UnknownColumns;

                    if (!Imported.bSucceeded)
                    {
                        return Agent::FToolResult::Error(Imported.FailureReason.empty()
                            ? FString("The CSV could not be imported.") : Imported.FailureReason);
                    }

                    MarkDirty(Table);

                    return Agent::FToolResult::Ok(Lumina::Format("Imported {} row(s), skipped {}, {} cell problem(s).{}",
                        Out.RowCount, Out.Skipped, Out.Errors.size(), GDataTableSaveHint));
                });
        }

        void RegisterExportCsv(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SExportCsvParams, SExportCsvResult>(
                Owner, "datatable.export_csv",
                "Write every row as CSV text, in the shape datatable.import_csv takes back.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SExportCsvParams& In, SExportCsvResult& Out)
                {
                    CDataTable* Table = nullptr;
                    FString Error;
                    if (!ResolveTable(In.Asset, Table, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Text = DataTableCSV::ExportText(Table);
                    return Agent::FToolResult::Ok(Out.Text);
                });
        }
    }

    void RegisterDataTableTools(FStringView Owner)
    {
        RegisterDescribeDataTable(Owner);
        RegisterGetRow(Owner);
        RegisterSetRowProperty(Owner);
        RegisterAddRow(Owner);
        RegisterRemoveRow(Owner);
        RegisterRenameRow(Owner);
        RegisterImportCsv(Owner);
        RegisterExportCsv(Owner);
    }
}
