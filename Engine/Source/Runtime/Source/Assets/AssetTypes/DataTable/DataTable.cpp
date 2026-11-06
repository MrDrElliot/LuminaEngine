#include "RuntimePCH.h"
#include "DataTable.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Containers/StringFormat.h"

#include <atomic>

namespace Lumina
{
    CStruct* CDataTable::GetRowStruct() const
    {
        return RowStructCache.Resolve(RowStructName);
    }

    void CDataTable::SetRowStruct(CStruct* InStruct)
    {
        ClearRows();

        RowStructName = InStruct != nullptr ? StructIdentity(InStruct) : FName();
        RowStructCache.Set(InStruct, RowStructName);
    }

    bool CDataTable::RowsAre(const CStruct* Base) const
    {
        const CStruct* RowStruct = GetRowStruct();
        return RowStruct != nullptr && RowStruct->IsChildOf(Base);
    }

    bool CDataTable::RowsAreNamed(const FName& BaseName) const
    {
        return !BaseName.IsNone() && RowsAre(ResolveStructByName(BaseName));
    }

    int32 CDataTable::FindRowIndex(const FName& RowName) const
    {
        for (int32 i = 0; i < Rows.Num(); ++i)
        {
            if (Rows[i].Name == RowName)
            {
                return i;
            }
        }
        return Constants::kIndexNone;
    }

    FName CDataTable::GetRowNameAt(int32 Index) const
    {
        return (Index >= 0 && Index < Rows.Num()) ? Rows[Index].Name : FName();
    }

    int32 CDataTable::GetRowStructSize() const
    {
        const CStruct* Struct = GetRowStruct();
        return Struct ? (int32)Struct->GetSize() : 0;
    }

    const void* CDataTable::FindRowHinted(const FName& RowName, int32& InOutIndex) const
    {
        if (InOutIndex < 0 || InOutIndex >= Rows.Num() || Rows[InOutIndex].Name != RowName)
        {
            InOutIndex = FindRowIndex(RowName);
        }
        if (InOutIndex == Constants::kIndexNone || Rows[InOutIndex].Value.GetScriptStruct() != GetRowStruct())
        {
            return nullptr;
        }
        return Rows[InOutIndex].Value.GetMemory();
    }

    const void* CDataTable::GetRowMemoryAt(int32 Index) const
    {
        if (Index < 0 || Index >= GetRowCount() || Rows[Index].Value.GetScriptStruct() != GetRowStruct())
        {
            return nullptr;
        }
        return Rows[Index].Value.GetMemory();
    }

    const void* CDataTable::FindRow(const FName& RowName) const
    {
        const int32 Index = FindRowIndex(RowName);
        if (Index == Constants::kIndexNone || Rows[Index].Value.GetScriptStruct() != GetRowStruct())
        {
            return nullptr;
        }
        return Rows[Index].Value.GetMemory();
    }

    void CDataTable::PostPropertyChange(FProperty* ChangedProperty)
    {
        Super::PostPropertyChange(ChangedProperty);

        // A direct edit of the name skips SetRowStruct, and rows of the old type must not be read as the new one.
        if (ChangedProperty != nullptr && ChangedProperty->GetPropertyName() == FName("RowStructName"))
        {
            CStruct* RowStruct = GetRowStruct();
            Rows.RemoveAll([RowStruct](const SDataTableRow& Row)
            {
                return Row.Value.GetScriptStruct() != RowStruct;
            });
        }
    }

    int32 CDataTable::AddRow(const FName& RowName)
    {
        CStruct* Struct = GetRowStruct();

        // A type outside the row hierarchy would quietly break every reader of the stored value.
        if (Struct == nullptr || !Struct->IsChildOf(SDataTableRowBase::StaticStruct()))
        {
            return Constants::kIndexNone;
        }

        SDataTableRow& Row = Rows.EmplaceGetRef();
        Row.Name = RowName;
        Row.Value.InitializeAs(Struct);

        return Rows.Num() - 1;
    }

    void CDataTable::RemoveRow(int32 Index)
    {
        if (Index < 0 || Index >= Rows.Num())
        {
            return;
        }

        // Ordered RemoveAt, not RemoveAtSwap, since row order is authored and visible in the editor.
        Rows.RemoveAt(Index);
    }

    void CDataTable::MoveRow(int32 From, int32 To)
    {
        const int32 Count = Rows.Num();
        if (From == To || From < 0 || From >= Count || To < 0 || To >= Count)
        {
            return;
        }

        SDataTableRow Moved = Move(Rows[From]);
        Rows.RemoveAt(From);
        Rows.Insert(Move(Moved), To);
    }

    void CDataTable::ClearRows()
    {
        Rows.Reset();
    }

    CStruct* SDataTableRowHandle::GetRowStruct() const
    {
        return DataTable != nullptr ? DataTable->GetRowStruct() : nullptr;
    }

    const void* SDataTableRowHandle::GetRowMemory() const
    {
        if (IsNull())
        {
            return nullptr;
        }

        // Relaxed, since a stale or torn index is only a hint and fails the name check like any other miss.
        std::atomic_ref<int32> Cached(CachedRowIndex);
        int32 Index = Cached.load(std::memory_order_relaxed);
        const void* Row = DataTable->FindRowHinted(RowName, Index);
        Cached.store(Index, std::memory_order_relaxed);
        return Row;
    }

    FName CDataTable::MakeUniqueRowName(const FName& Base) const
    {
        if (FindRowIndex(Base) == Constants::kIndexNone)
        {
            return Base;
        }

        const FString BaseText = Base.ToString();
        for (int32 Suffix = 1; Suffix < 100000; ++Suffix)
        {
            const FName Candidate(BaseText + "_" + *Format("{}", Suffix));
            if (FindRowIndex(Candidate) == Constants::kIndexNone)
            {
                return Candidate;
            }
        }

        return Base;
    }
}
