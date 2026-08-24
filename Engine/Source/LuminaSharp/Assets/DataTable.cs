using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using LuminaSharp;

namespace Lumina;

/// Handwritten extensions to the reflected <see cref="CDataTable"/> wrapper, adding typed row lookup.
public unsafe partial class CDataTable
{
    /// Every row name in authored order.
    public List<string> RowNames
    {
        get
        {
            int Count = GetRowCount();
            var Names = new List<string>(Count);
            for (int Index = 0; Index < Count; ++Index)
            {
                Names.Add(GetRowNameAt(Index));
            }
            return Names;
        }
    }

    /// The row named RowName as a live view, or null. T must be the table's row type exactly.
    public T? FindRow<T>(string RowName) where T : NativeStruct
    {
        if (!RowTypeIs<T>())
        {
            return null;
        }
        IntPtr Row = Native.DataTableFindRow(Handle, RowName);
        return Row == IntPtr.Zero ? null : Wrapper<T>.Create(Row);
    }

    /// The row at Index as a live view, or null. T must be the table's row type exactly.
    public T? RowAt<T>(int Index) where T : NativeStruct
    {
        if (!RowTypeIs<T>())
        {
            return null;
        }
        IntPtr Row = Native.DataTableGetRowAt(Handle, Index);
        return Row == IntPtr.Zero ? null : Wrapper<T>.Create(Row);
    }

    /// Copies the row named RowName into a blittable value struct. False when absent or T does not match.
    public bool TryGetRow<T>(string RowName, out T Row) where T : unmanaged
    {
        Row = default;
        if (!RowTypeMatchesValue<T>())
        {
            return false;
        }
        IntPtr Memory = Native.DataTableFindRow(Handle, RowName);
        if (Memory == IntPtr.Zero)
        {
            return false;
        }
        Row = Unsafe.ReadUnaligned<T>((void*)Memory);
        return true;
    }

    // Exact match, mirroring the C++ FindDataTableRow bound, so a near-miss type cannot reinterpret fields.
    private bool RowTypeIs<T>() where T : NativeStruct
    {
        return string.Equals(RowStructName, NativeTypeName.Of<T>(), StringComparison.Ordinal);
    }

    // A value read also has to agree on size, or a layout drift would copy the wrong bytes silently.
    private bool RowTypeMatchesValue<T>() where T : unmanaged
    {
        if (!string.Equals(RowStructName, typeof(T).Name, StringComparison.Ordinal))
        {
            return false;
        }

        int NativeSize = GetRowStructSize();
        if (NativeSize != Unsafe.SizeOf<T>())
        {
            Native.Log(ELogLevel.Error,
                $"DataTable row '{RowStructName}' is {NativeSize} bytes natively but {Unsafe.SizeOf<T>()} as "
                + $"'{typeof(T).FullName}'; refusing the read. The C# type has drifted from the row struct.");
            return false;
        }
        return true;
    }
}

/// Handwritten extensions to the reflected <see cref="SDataTableRowHandle"/> wrapper.
public unsafe partial class SDataTableRowHandle
{
    /// True when nothing is referenced at all, as opposed to referencing a row that fails to resolve.
    public bool IsNull => DataTable == null || string.IsNullOrEmpty(RowName);

    /// The referenced row as a live view, or null when the handle is empty or the name is not in the table.
    public T? Resolve<T>() where T : NativeStruct
    {
        IntPtr Row = Native.DataTableHandleGetRowMemory(Handle);
        if (Row == IntPtr.Zero || DataTable is not { } Table
            || !string.Equals(Table.RowStructName, NativeTypeName.Of<T>(), StringComparison.Ordinal))
        {
            return null;
        }
        return Wrapper<T>.Create(Row);
    }
}
