using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// One argument of a data-event-* call, as UTF-8.
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("FUIArg")]
internal struct UIArg
{
    public IntPtr Ptr;
    public int Len;
}

// What the view wrote into a two-way bound variable; Str is set for a string and valid only for the call.
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("FUIDataSet")]
internal unsafe struct UIDataSet
{
    public int Field;
    public int Type;
    public double Number;
    public IntPtr Str;
    public int StrLen;
}

// A data-event-* command the view fired.
[StructLayout(LayoutKind.Sequential)]
[NativeLayout("FUIDataEvent")]
internal unsafe struct UIDataEvent
{
    public int CommandId;
    public int ArgCount;
    public UIArg* Args;
}

internal sealed unsafe class UIDataSetClosure : ScriptClosure
{
    public required UIDataModel Model;

    public override ulong Invoke(ulong Payload)
    {
        UIDataSet* Set = (UIDataSet*)Payload;
        Model.ApplyFromNative(Set->Field, Set->Number, Set->Str, Set->StrLen);
        return 0;
    }
}

internal sealed unsafe class UIDataEventClosure : ScriptClosure
{
    public required UIDataModel Model;

    public override ulong Invoke(ulong Payload)
    {
        UIDataEvent* Fired = (UIDataEvent*)Payload;
        Model.InvokeCommand(Fired->CommandId, Fired->ArgCount, Fired->Args);
        return 0;
    }
}

// Binds a ViewModel's or CUIScript's Bind members to a named RmlUi data model, comparing them once a frame so plain assignments reach the view.
public sealed unsafe class UIDataModel : IDisposable
{
    private static readonly Dictionary<(ulong World, string Name), UIDataModel> Registry = new();

    private readonly ulong _world;
    private readonly string _name;
    private readonly object _target;
    private readonly UIModelShape _shape;
    private Lumina.SUIDataModel _native;

    // Native field id per scalar and per list, parallel to the shape's lists. Negative when the bind failed.
    private readonly int[] _scalarField;
    private readonly int[] _listField;

    // What the view last received, so a frame where nothing changed pushes nothing.
    private readonly double[] _lastNumber;
    private readonly string?[] _lastText;
    private readonly long[] _lastList;

    private readonly Dictionary<string, int> _scalarByName = new(StringComparer.Ordinal);
    private readonly Dictionary<string, int> _listByName = new(StringComparer.Ordinal);

    // Set while applying a value the view sent, so the property setter's Set() does not echo it back.
    private bool _applyingFromNative;

    public UIDataModel(Lumina.CWorld World, string Name, ViewModel Target)
        : this(World.WorldHandle, Name, Target)
    {
    }

    internal UIDataModel(ulong World, string Name, object Target)
    {
        _world = World;
        _name = Name;
        _target = Target;
        _shape = UIModelShape.Of(Target.GetType());
        _scalarField = new int[_shape.Scalars.Count];
        _listField = new int[_shape.Lists.Count];
        _lastNumber = new double[_shape.Scalars.Count];
        _lastText = new string?[_shape.Scalars.Count];
        _lastList = new long[_shape.Lists.Count];

        _native = UIWorld.FromHandle(World).UI.CreateDataModel(Name,
            ScriptCallback.Bind(new UIDataSetClosure { Model = this }), ScriptCallback.Bind(new UIDataEventClosure { Model = this }));
        if (!_native.IsValid)
        {
            return;
        }

        BindMembers();
        if (_target is ViewModel Model)
        {
            Model.Binding = this;
        }
        lock (Registry)
        {
            Registry[(World, Name)] = this;
        }
        PushAll();
    }

    // False if the model failed to register or has been disposed.
    public bool IsValid => _native.IsValid;

    public string Name => _name;

    // The view-model this binding drives, or null when the target is a CUIScript.
    public ViewModel? ViewModel => _target as ViewModel;

    public object Target => _target;

    internal ulong World => _world;

    public static UIDataModel? Find(Lumina.CWorld World, string Name) => Find(World.WorldHandle, Name);

    internal static UIDataModel? Find(ulong World, string Name)
    {
        lock (Registry)
        {
            return Registry.TryGetValue((World, Name), out UIDataModel? Model) ? Model : null;
        }
    }

    private void BindMembers()
    {
        for (int Index = 0; Index < _shape.Scalars.Count; ++Index)
        {
            UIModelShape.FScalar Scalar = _shape.Scalars[Index];
            _scalarField[Index] = Lumina.CUILibrary.BindScalar(_native, Scalar.Name, Scalar.VarType);
            _scalarByName[Scalar.Name] = Index;
            _scalarByName[Scalar.MemberName] = Index;
        }

        for (int Index = 0; Index < _shape.Lists.Count; ++Index)
        {
            UIModelShape.FList List = _shape.Lists[Index];
            int Field = Lumina.CUILibrary.BindList(_native, List.Name);
            _listField[Index] = Field;
            if (Field >= 0)
            {
                foreach (UIModelShape.FListItem Item in List.Items)
                {
                    Lumina.CUILibrary.BindListMember(_native, Field, Item.Name);
                }
            }
            _listByName[List.Name] = Index;
            _listByName[List.MemberName] = Index;
        }

        for (int Index = 0; Index < _shape.Commands.Count; ++Index)
        {
            Lumina.CUILibrary.BindCommand(_native, _shape.Commands[Index].Name, Index);
        }
    }

    // Pushes one member by its bound or member name, for ViewModel.Set and for a collection edited in place.
    internal void OnPropertyChanged(string Name)
    {
        if (_applyingFromNative || !_native.IsValid)
        {
            return;
        }
        if (_scalarByName.TryGetValue(Name, out int Scalar))
        {
            SyncScalar(Scalar, bForce: true, bMarkDirty: true);
        }
        else if (_listByName.TryGetValue(Name, out int List))
        {
            PushList(List, force: true);
        }
    }

    // Re-push every bound member and mark the whole model dirty.
    public void PushAll()
    {
        if (!_native.IsValid)
        {
            return;
        }
        for (int Index = 0; Index < _shape.Scalars.Count; ++Index)
        {
            SyncScalar(Index, bForce: true, bMarkDirty: false);
        }
        for (int Index = 0; Index < _shape.Lists.Count; ++Index)
        {
            PushList(Index, force: true);
        }
        Lumina.CUILibrary.MarkAllDirty(_native);
    }

    // Sends whatever changed since the last push. Runs for every model of a world just before its UI updates.
    internal void Poll()
    {
        if (!_native.IsValid)
        {
            return;
        }
        for (int Index = 0; Index < _shape.Scalars.Count; ++Index)
        {
            SyncScalar(Index, bForce: false, bMarkDirty: true);
        }
        for (int Index = 0; Index < _shape.Lists.Count; ++Index)
        {
            PushList(Index, force: false);
        }
    }

    // Reads one member and sends it when it differs from what the view holds, or always when forced.
    private void SyncScalar(int Index, bool bForce, bool bMarkDirty)
    {
        UIModelShape.FScalar Scalar = _shape.Scalars[Index];
        int Field = _scalarField[Index];
        try
        {
            if (Scalar.ReadText != null)
            {
                string Text = Scalar.ReadText(_target) ?? string.Empty;
                if (!bForce && string.Equals(Text, _lastText[Index], StringComparison.Ordinal))
                {
                    return;
                }
                _lastText[Index] = Text;
                if (Field >= 0)
                {
                    Lumina.CUILibrary.SetString(_native, Field, Text);
                }
            }
            else
            {
                double Number = Scalar.ReadNumber!(_target);
                if (!bForce && Number.Equals(_lastNumber[Index]))
                {
                    return;
                }
                _lastNumber[Index] = Number;
                if (Field >= 0)
                {
                    Lumina.CUILibrary.SetNumber(_native, Field, Number);
                }
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return;
        }

        if (bMarkDirty && Field >= 0)
        {
            Lumina.CUILibrary.MarkDirty(_native, Field);
        }
    }

    private void PushList(int Index, bool force)
    {
        int Field = _listField[Index];
        if (Field < 0)
        {
            return;
        }
        UIModelShape.FList List = _shape.Lists[Index];
        object? Collection;
        try
        {
            Collection = List.Get(_target);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return;
        }

        long Signature = List.Signature(Collection);
        if (!force && Signature == _lastList[Index])
        {
            return;
        }
        _lastList[Index] = Signature;

        var Rows = new List<object>();
        if (Collection is IEnumerable Items)
        {
            foreach (object? Item in Items)
            {
                if (Item != null)
                {
                    Rows.Add(Item);
                }
            }
        }

        Lumina.CUILibrary.ResizeList(_native, Field, Rows.Count);
        for (int Row = 0; Row < Rows.Count; ++Row)
        {
            for (int Col = 0; Col < List.Items.Length; ++Col)
            {
                Lumina.CUILibrary.SetListCell(_native, Field, Row, Col, ToCell(List.Items[Col].Get(Rows[Row])));
            }
        }
        Lumina.CUILibrary.MarkListDirty(_native, Field);
    }

    // ---- native -> managed ----

    internal void ApplyFromNative(int Field, double Number, IntPtr Str, int StrLen)
    {
        int Index = Array.IndexOf(_scalarField, Field);
        if (Index < 0)
        {
            return;
        }
        UIModelShape.FScalar Scalar = _shape.Scalars[Index];
        if (Scalar.Set == null)
        {
            return;
        }

        object Value = Scalar.VarType == Lumina.EUIVarType.String
            ? Interop.GetString((byte*)Str, StrLen)
            : ConvertNumber(Scalar.ValueType, Number);

        _applyingFromNative = true;
        try
        {
            using var Scope = EnterTarget();
            Scalar.Set(_target, Value);
            if (Value is string Text)
            {
                _lastText[Index] = Text;
            }
            else
            {
                _lastNumber[Index] = Number;
            }
        }
        catch (Exception Exception)
        {
            Report(Scalar.MemberName, Exception);
        }
        finally
        {
            _applyingFromNative = false;
        }
    }

    internal void InvokeCommand(int CommandId, int ArgCount, UIArg* Args)
    {
        if (CommandId < 0 || CommandId >= _shape.Commands.Count)
        {
            return;
        }
        UIModelShape.FCommand Command = _shape.Commands[CommandId];

        object?[] Call = Command.Parameters.Length == 0 ? Array.Empty<object?>() : new object?[Command.Parameters.Length];
        for (int Index = 0; Index < Call.Length; ++Index)
        {
            string Arg = Index < ArgCount ? Interop.GetString((byte*)Args[Index].Ptr, Args[Index].Len) : string.Empty;
            Call[Index] = ConvertArg(Arg, Command.Parameters[Index].ParameterType);
        }

        try
        {
            using var Scope = EnterTarget();
            Command.Method.Invoke(_target, Call);
        }
        catch (TargetInvocationException Thrown) when (Thrown.InnerException != null)
        {
            Report(Command.Name, Thrown.InnerException);
        }
    }

    // A CUIScript's handlers run inside its own script context, the same as its other callbacks.
    private Engine.Scope EnterTarget()
    {
        return _target is Lumina.CEntityScript Script && !Script.Entity.IsNull ? Engine.Push(Script.World, Script.Entity, Script) : Engine.Snapshot();
    }

    private void Report(string Member, Exception Exception)
    {
        if (_target is Lumina.CEntityScript)
        {
            NativeBindings.ScriptEventException(_target, Member, Exception);
        }
        else
        {
            Interop.LogException(Exception);
        }
    }

    // Called by native once a frame per world, just before that world's UI updates.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void PollUIDataModels(ulong World)
    {
        try
        {
            // Copied out, since a model's getters may create or dispose models mid-poll.
            int Count;
            UIDataModel[] Models;
            lock (Registry)
            {
                Count = Registry.Count;
                if (Count == 0)
                {
                    return;
                }
                Models = System.Buffers.ArrayPool<UIDataModel>.Shared.Rent(Count);
                Registry.Values.CopyTo(Models, 0);
            }
            try
            {
                for (int Index = 0; Index < Count; ++Index)
                {
                    if (Models[Index]._world == World)
                    {
                        Models[Index].Poll();
                    }
                }
            }
            finally
            {
                System.Buffers.ArrayPool<UIDataModel>.Shared.Return(Models, clearArray: true);
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    // Drops the models belonging to one world as it tears down, since its Rml context goes with it.
    internal static void RemoveForWorld(ulong World)
    {
        foreach (UIDataModel Model in SnapshotRegistry())
        {
            if (Model._world == World)
            {
                Model.Dispose();
            }
        }
    }

    // Before a script load context unloads, since a live model roots the user types it binds.
    public static void DisposeAll()
    {
        foreach (UIDataModel Model in SnapshotRegistry())
        {
            Model.Dispose();
        }
        lock (Registry)
        {
            Registry.Clear();
        }
    }

    private static List<UIDataModel> SnapshotRegistry()
    {
        lock (Registry)
        {
            return new List<UIDataModel>(Registry.Values);
        }
    }

    public void Dispose()
    {
        lock (Registry)
        {
            if (Registry.TryGetValue((_world, _name), out UIDataModel? Registered) && ReferenceEquals(Registered, this))
            {
                Registry.Remove((_world, _name));
            }
        }
        if (_native.IsValid)
        {
            Lumina.CUILibrary.DestroyDataModel(_native);
            _native = default;
        }
        if (_target is ViewModel Model && ReferenceEquals(Model.Binding, this))
        {
            Model.Binding = null;
        }
    }

    // ---- conversions ----

    private static string ToCell(object? Value)
    {
        return Value == null ? string.Empty : (Convert.ToString(Value, CultureInfo.InvariantCulture) ?? string.Empty);
    }

    private static object ConvertNumber(Type Type, double Number)
    {
        if (Type.IsEnum) return Enum.ToObject(Type, (long)Number);
        if (Type == typeof(bool)) return Number != 0.0;
        if (Type == typeof(float)) return (float)Number;
        if (Type == typeof(double)) return Number;
        if (Type == typeof(int)) return (int)Number;
        if (Type == typeof(long)) return (long)Number;
        if (Type == typeof(short)) return (short)Number;
        if (Type == typeof(byte)) return (byte)Number;
        if (Type == typeof(sbyte)) return (sbyte)Number;
        if (Type == typeof(uint)) return (uint)Number;
        if (Type == typeof(ushort)) return (ushort)Number;
        if (Type == typeof(ulong)) return (ulong)Number;
        return Convert.ChangeType(Number, Type, CultureInfo.InvariantCulture);
    }

    private static object? ConvertArg(string Value, Type Type)
    {
        try
        {
            if (Type == typeof(string)) return Value;
            if (Type == typeof(bool)) return Value == "1" || Value.Equals("true", StringComparison.OrdinalIgnoreCase);
            if (Type.IsEnum) return Enum.Parse(Type, Value, true);
            return Convert.ChangeType(Value, Type, CultureInfo.InvariantCulture);
        }
        catch
        {
            return Type.IsValueType ? Activator.CreateInstance(Type) : null;
        }
    }
}
