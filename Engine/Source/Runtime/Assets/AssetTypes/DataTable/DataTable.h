#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/InstancedStruct.h"
#include "DataTable.generated.h"

namespace Lumina
{
    /** Base every data table row type derives from, as UE's FTableRowBase is.
     *
     *  It carries no fields. It is the compile-time bound for TInstancedStruct, and it is what the
     *  row-struct picker filters on: without it the picker would offer every reflected struct in the
     *  build, most of which make no sense as a table row.
     *
     *  Declare row structs in whichever module owns them, including a game or plugin module -- those
     *  workspaces pull the engine's reflected types in as reference-only input, so a base declared
     *  here resolves correctly from there.
     */
    REFLECT()
    struct RUNTIME_API SDataTableRowBase
    {
        GENERATED_BODY()
    };

    /** One keyed row. Name and value live together rather than in parallel arrays: pairing them
     *  removes a whole class of desync bug a partially-loaded asset could otherwise produce. */
    REFLECT()
    struct RUNTIME_API SDataTableRow
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        FName Name;

        PROPERTY(Editable)
        TInstancedStruct<SDataTableRowBase> Value;
    };

    /** A table of uniformly-typed rows, each an instance of RowStruct, keyed by name.
     *
     *  Rows hold instanced structs rather than a raw byte block: the struct is only known at runtime,
     *  and InstancedStruct already owns construction, destruction, copying and serialization for a
     *  reflected type. The cost is one allocation per row, which is the right trade for an asset that
     *  is authored in the editor and read by name at runtime.
     */
    REFLECT(MinimalAPI)
    class CDataTable : public CObject
    {
        GENERATED_BODY()
    public:

        /** Name of the type every row is an instance of.
         *
         *  Stored as a name rather than a struct reference: a CStruct is a reflection object created
         *  at module init, not a serializable asset, so a pointer to one cannot be written to a
         *  package. Read it through GetRowStruct(), which resolves and caches. */
        PROPERTY()
        FName RowStructName;

        PROPERTY()
        TVector<SDataTableRow> Rows;

        /** The resolved row type, or null if unset or its module is not loaded. */
        RUNTIME_API NODISCARD CStruct* GetRowStruct() const;

        /** Points the table at a new row type. Drops every existing row: their fields belong to the
         *  old struct, and reinterpreting those bytes as a different layout is not a conversion. */
        RUNTIME_API void SetRowStruct(CStruct* InStruct);

        RUNTIME_API NODISCARD int32 GetRowCount() const { return (int32)Rows.size(); }

        /** Index of RowName, or INDEX_NONE. Linear: tables are authored small and read by name rarely
         *  enough that an index would cost more to keep coherent than it saves. */
        RUNTIME_API NODISCARD int32 FindRowIndex(const FName& RowName) const;

        /** Row memory as RowStruct, or null when the name is unknown. */
        RUNTIME_API NODISCARD const void* FindRow(const FName& RowName) const;

        /** Appends a default-constructed row. Returns its index, or INDEX_NONE with no RowStruct set. */
        RUNTIME_API int32 AddRow(const FName& RowName);

        RUNTIME_API void RemoveRow(int32 Index);

        /** Drops every row. Called when RowStruct changes, since the old rows describe a different type. */
        RUNTIME_API void ClearRows();

        /** Unique "NewRow", "NewRow_1", ... for a table that already has rows. */
        RUNTIME_API NODISCARD FName MakeUniqueRowName(const FName& Base) const;

    private:

        /** Resolution result for RowStructName. Not serialized: reflection objects live at different
         *  addresses each run, and a stale pointer here would outlive its module. */
        mutable CStruct* CachedRowStruct = nullptr;
        mutable FName CachedRowStructName;
    };

    /** Typed lookup. Returns null unless the table's row type actually is T -- a mismatched cast here
     *  would reinterpret unrelated fields rather than fail. A free function because the reflector does
     *  not parse member templates. */
    template <typename T>
    const T* FindDataTableRow(const CDataTable* Table, const FName& RowName)
    {
        if (Table == nullptr || Table->GetRowStruct() != T::StaticStruct())
        {
            return nullptr;
        }
        return static_cast<const T*>(Table->FindRow(RowName));
    }
}
