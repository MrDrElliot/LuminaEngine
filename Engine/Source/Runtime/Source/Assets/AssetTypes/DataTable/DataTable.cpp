#include "RuntimePCH.h"
#include "DataTable.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

namespace Lumina
{
    CStruct* CDataTable::GetRowStruct() const
    {
        return RowStructCache.Resolve(RowStructName);
    }

    void CDataTable::SetRowStruct(CStruct* InStruct)
    {
        ClearRows();

        RowStructName = InStruct != nullptr ? DataStructIdentity(InStruct) : FName();
        RowStructCache.Set(InStruct, RowStructName);
    }

    int32 CDataTable::FindRowIndex(const FName& RowName) const
    {
        for (int32 i = 0; i < (int32)Rows.size(); ++i)
        {
            if (Rows[i].Name == RowName)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    const void* CDataTable::FindRow(const FName& RowName) const
    {
        const int32 Index = FindRowIndex(RowName);
        return Index != INDEX_NONE ? Rows[Index].Value.GetMemory() : nullptr;
    }

    int32 CDataTable::AddRow(const FName& RowName)
    {
        CStruct* Struct = GetRowStruct();

        // TInstancedStruct's contract is that the stored value is always readable as
        // SDataTableRowBase; a type outside that hierarchy would quietly break every reader.
        if (Struct == nullptr || !Struct->IsChildOf(SDataTableRowBase::StaticStruct()))
        {
            return INDEX_NONE;
        }

        SDataTableRow& Row = Rows.emplace_back();
        Row.Name = RowName;
        Row.Value.InitializeAs(Struct);

        return (int32)Rows.size() - 1;
    }

    void CDataTable::RemoveRow(int32 Index)
    {
        if (Index < 0 || Index >= (int32)Rows.size())
        {
            return;
        }

        // Ordered erase, not swap-and-pop: row order is authored and visible in the editor.
        Rows.erase(Rows.begin() + Index);
    }

    void CDataTable::MoveRow(int32 From, int32 To)
    {
        const int32 Count = (int32)Rows.size();
        if (From == To || From < 0 || From >= Count || To < 0 || To >= Count)
        {
            return;
        }

        SDataTableRow Moved = eastl::move(Rows[From]);
        Rows.erase(Rows.begin() + From);
        Rows.insert(Rows.begin() + To, eastl::move(Moved));
    }

    void CDataTable::ClearRows()
    {
        Rows.clear();
    }

    CStruct* SDataTableRowHandle::GetRowStruct() const
    {
        return DataTable != nullptr ? DataTable->GetRowStruct() : nullptr;
    }

    const void* SDataTableRowHandle::GetRowMemory() const
    {
        return IsNull() ? nullptr : DataTable->FindRow(RowName);
    }

    FName CDataTable::MakeUniqueRowName(const FName& Base) const
    {
        if (FindRowIndex(Base) == INDEX_NONE)
        {
            return Base;
        }

        const FString BaseText = Base.ToString();
        for (int32 Suffix = 1; Suffix < 100000; ++Suffix)
        {
            const FName Candidate(BaseText + "_" + eastl::to_string(Suffix).c_str());
            if (FindRowIndex(Candidate) == INDEX_NONE)
            {
                return Candidate;
            }
        }

        return Base;
    }
}
