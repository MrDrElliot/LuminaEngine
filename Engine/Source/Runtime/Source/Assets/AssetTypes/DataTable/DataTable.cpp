#include "RuntimePCH.h"
#include "DataTable.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"

namespace Lumina
{
    CStruct* CDataTable::GetRowStruct() const
    {
        // Re-resolve whenever the name moves; the cache is keyed on the name it was resolved from so
        // an edit (or a load) cannot leave a pointer to the previous type behind.
        if (CachedRowStructName != RowStructName)
        {
            CachedRowStructName = RowStructName;
            CachedRowStruct = RowStructName.IsNone() ? nullptr : FindObject<CStruct>(RowStructName);
        }

        return CachedRowStruct;
    }

    void CDataTable::SetRowStruct(CStruct* InStruct)
    {
        ClearRows();

        RowStructName = InStruct != nullptr ? InStruct->GetName() : FName();
        CachedRowStructName = RowStructName;
        CachedRowStruct = InStruct;
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

    void CDataTable::ClearRows()
    {
        Rows.clear();
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
