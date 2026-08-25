using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// Scalar wire type for a bound variable; mirrors native RmlUi::EUIVarType.
internal enum UIVarType
{
    Bool = 0,
    Int = 1,
    Float = 2,
    Double = 3,
    String = 4,
}

/// One argument of a data-event-* call; mirrors native RmlUi::FUIArg (UTF-8 ptr+len).
[StructLayout(LayoutKind.Sequential)]
internal struct UIArg
{
    public IntPtr Ptr;
    public int Len;
}

/// A live MVVM binding between a ViewModel and an RmlUi data model on a world's UI context. Dispose it (before the world tears down) to remove the model and free the callback.
public sealed unsafe class UIDataModel : IDisposable
{
    // Game-thread only, so a plain dictionary is fine. Lets World.UI.GetModel re-fetch a model by name.
    private static readonly Dictionary<(ulong World, string Name), UIDataModel> Registry = new();

    private readonly ulong _world;
    private readonly string _name;
    private readonly ViewModel _viewModel;
    private IntPtr _native;          // FManagedDataModel*
    private GCHandle _self;

    // Scalar fields; list index == native field id (dense, assigned in registration order).
    private readonly List<ScalarField> _fields = new();
    private readonly Dictionary<string, int> _fieldByName = new(StringComparer.Ordinal);

    // List fields; their own id space (native list field id).
    private readonly List<ListField> _lists = new();
    private readonly Dictionary<string, int> _listByName = new(StringComparer.Ordinal);

    private readonly List<Command> _commands = new();

    // Set while applying a value received FROM the view, so the property setter's Set() does not echo it back.
    private bool _applyingFromNative;

    private readonly struct ScalarField
    {
        public readonly PropertyInfo Property;
        public readonly Func<object, object?> Get;
        public readonly Action<object, object?>? Set;
        public readonly UIVarType Type;

        public ScalarField(PropertyInfo property, Func<object, object?> get, Action<object, object?>? set, UIVarType type)
        {
            Property = property;
            Get = get;
            Set = set;
            Type = type;
        }
    }

    private readonly struct ItemMember
    {
        public readonly string Name;
        public readonly Func<object, object?> Get;

        public ItemMember(string name, Func<object, object?> get)
        {
            Name = name;
            Get = get;
        }
    }

    private sealed class ListField
    {
        public Func<object, object?> Get = null!;   // reads the collection off the view-model
        public int Field;                            // native list field id
        public ItemMember[] Members = Array.Empty<ItemMember>();
    }

    private readonly struct Command
    {
        public readonly MethodInfo Method;
        public readonly ParameterInfo[] Parameters;

        public Command(MethodInfo method)
        {
            Method = method;
            Parameters = method.GetParameters();
        }
    }

    internal UIDataModel(ulong World, string Name, ViewModel Model)
    {
        _world = World;
        _name = Name;
        _viewModel = Model;
        _self = GCHandle.Alloc(this);

        _native = Native.UI_CreateDataModel(World, Name, GCHandle.ToIntPtr(_self), SetThunkPtr, EventThunkPtr);
        if (_native == IntPtr.Zero)
        {
            _self.Free();
            return;
        }

        BindMembers();
        _viewModel.Binding = this;
        Registry[(World, Name)] = this;
        PushAll();   // seed the view with the initial values
    }

    /// False if the model failed to register or has been disposed.
    public bool IsValid => _native != IntPtr.Zero;

    /// The data-model name on the world's UI context.
    public string Name => _name;

    /// The view-model this binding drives.
    public ViewModel ViewModel => _viewModel;

    /// Re-fetch a registered model by name (backs UI.GetModel); null if none.
    internal static UIDataModel? Find(ulong World, string Name)
        => Registry.TryGetValue((World, Name), out UIDataModel? Model) ? Model : null;

    private void BindMembers()
    {
        Type Type = _viewModel.GetType();

        foreach (PropertyInfo Property in Type.GetProperties(BindingFlags.Public | BindingFlags.Instance))
        {
            BindAttribute? Attribute = Property.GetCustomAttribute<BindAttribute>();
            if (Attribute == null || Property.GetMethod == null)
            {
                continue;
            }
            string Name = Attribute.Name ?? Property.Name;

            if (TryMapType(Property.PropertyType, out UIVarType VarType))
            {
                int Field = Native.UI_ModelBindScalar(_native, Name, (int)VarType);
                if (Field < 0)
                {
                    continue;
                }
                _fields.Add(new ScalarField(Property, PropertyAccessor.Getter(Property), PropertyAccessor.Setter(Property), VarType));
                _fieldByName[Name] = Field;
                _fieldByName[Property.Name] = Field;
            }
            else if (TryGetItemType(Property.PropertyType, out Type ItemType) && HasBindMembers(ItemType))
            {
                int Field = Native.UI_ModelBindList(_native, Name);
                if (Field < 0)
                {
                    continue;
                }
                ItemMember[] Members = BuildItemMembers(ItemType);
                foreach (ItemMember Member in Members)
                {
                    Native.UI_ModelBindListMember(_native, Field, Member.Name);
                }
                int ListIndex = _lists.Count;
                _lists.Add(new ListField { Get = PropertyAccessor.Getter(Property), Field = Field, Members = Members });
                _listByName[Name] = ListIndex;
                _listByName[Property.Name] = ListIndex;
            }
            else
            {
                Debug.LogWarning($"[UI] {Type.Name}.{Property.Name}: [Bind] type '{Property.PropertyType.Name}' is not bindable; skipped.");
            }
        }

        foreach (MethodInfo Method in Type.GetMethods(BindingFlags.Public | BindingFlags.Instance))
        {
            BindCommandAttribute? Attribute = Method.GetCustomAttribute<BindCommandAttribute>();
            if (Attribute == null)
            {
                continue;
            }
            string CommandName = Attribute.Name ?? Method.Name;
            int CommandId = _commands.Count;
            _commands.Add(new Command(Method));
            Native.UI_ModelBindCommand(_native, CommandName, CommandId);
        }
    }

    /// Push one property (scalar or list) to the view by name (called by ViewModel.Set).
    internal void OnPropertyChanged(string Name)
    {
        if (_applyingFromNative || _native == IntPtr.Zero)
        {
            return;
        }
        if (_fieldByName.TryGetValue(Name, out int Field))
        {
            WriteScalar(Field);
            Native.UI_ModelDirty(_native, Field);
        }
        else if (_listByName.TryGetValue(Name, out int ListIndex))
        {
            WriteList(ListIndex);
            Native.UI_ModelListDirty(_native, _lists[ListIndex].Field);
        }
    }

    /// Re-push every bound property (scalars + lists) and mark the whole model dirty.
    public void PushAll()
    {
        if (_native == IntPtr.Zero)
        {
            return;
        }
        for (int i = 0; i < _fields.Count; i++)
        {
            WriteScalar(i);
        }
        for (int i = 0; i < _lists.Count; i++)
        {
            WriteList(i);
        }
        Native.UI_ModelDirtyAll(_native);   // covers scalar AND list (custom) variables
    }

    private void WriteScalar(int Field)
    {
        ScalarField F = _fields[Field];
        object? Value = F.Get(_viewModel);
        if (F.Type == UIVarType.String)
        {
            Native.UI_ModelSetString(_native, Field, Value as string ?? string.Empty);
        }
        else
        {
            Native.UI_ModelSetNumber(_native, Field, ToNumber(Value, F.Type));
        }
    }

    private void WriteList(int ListIndex)
    {
        ListField List = _lists[ListIndex];
        object? Collection = List.Get(_viewModel);

        // Snapshot the rows (the source may be any IEnumerable).
        List<object> Rows = new();
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

        Native.UI_ModelListResize(_native, List.Field, Rows.Count);
        for (int Row = 0; Row < Rows.Count; Row++)
        {
            for (int Col = 0; Col < List.Members.Length; Col++)
            {
                Native.UI_ModelListSetCell(_native, List.Field, Row, Col, ToCell(List.Members[Col].Get(Rows[Row])));
            }
        }
    }

    // ---- native -> managed ----

    private void ApplyFromNative(int Field, double Number, IntPtr Str, int StrLen)
    {
        if (Field < 0 || Field >= _fields.Count)
        {
            return;
        }
        ScalarField F = _fields[Field];
        if (F.Set == null)
        {
            return;   // display-only property; ignore writebacks
        }

        object Value = F.Type == UIVarType.String
            ? (StrLen > 0 ? Marshal.PtrToStringUTF8(Str, StrLen) ?? string.Empty : string.Empty)
            : ConvertNumber(F.Property.PropertyType, Number);

        _applyingFromNative = true;
        try
        {
            F.Set(_viewModel, Value);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
        finally
        {
            _applyingFromNative = false;
        }
    }

    private void InvokeCommand(int CommandId, int ArgCount, UIArg* Args)
    {
        if (CommandId < 0 || CommandId >= _commands.Count)
        {
            return;
        }
        Command Cmd = _commands[CommandId];

        object?[] Call;
        if (Cmd.Parameters.Length == 0)
        {
            Call = Array.Empty<object?>();
        }
        else
        {
            Call = new object?[Cmd.Parameters.Length];
            for (int i = 0; i < Cmd.Parameters.Length; i++)
            {
                string Arg = i < ArgCount ? (Marshal.PtrToStringUTF8(Args[i].Ptr, Args[i].Len) ?? string.Empty) : string.Empty;
                Call[i] = ConvertArg(Arg, Cmd.Parameters[i].ParameterType);
            }
        }
        Cmd.Method.Invoke(_viewModel, Call);
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void SetThunk(IntPtr Context, int Field, int Type, double Number, IntPtr Str, int StrLen)
    {
        try
        {
            if (GCHandle.FromIntPtr(Context).Target is UIDataModel Self)
            {
                Self.ApplyFromNative(Field, Number, Str, StrLen);
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void EventThunk(IntPtr Context, int CommandId, int ArgCount, UIArg* Args)
    {
        try
        {
            if (GCHandle.FromIntPtr(Context).Target is UIDataModel Self)
            {
                Self.InvokeCommand(CommandId, ArgCount, Args);
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    private static readonly IntPtr SetThunkPtr =
        (IntPtr)(delegate* unmanaged[Cdecl]<IntPtr, int, int, double, IntPtr, int, void>)&SetThunk;

    private static readonly IntPtr EventThunkPtr =
        (IntPtr)(delegate* unmanaged[Cdecl]<IntPtr, int, int, UIArg*, void>)&EventThunk;

    /// <summary>Disposes every registered data model. Called before a script ALC unload as a safety net: a
    /// model whose owner forgot to Dispose() in OnDetach holds the user ViewModel (and PropertyAccessor
    /// delegates over user types), which would pin the collectible generation and leak the native model.</summary>
    // Drops the models belonging to one world as it tears down, since its Rml context goes with it.
    internal static void RemoveForWorld(ulong World)
    {
        foreach (UIDataModel Model in new List<UIDataModel>(Registry.Values))
        {
            if (Model._world == World)
            {
                Model.Dispose();
            }
        }
    }

    public static void DisposeAll()
    {
        foreach (UIDataModel Model in new List<UIDataModel>(Registry.Values))
        {
            Model.Dispose();
        }
        Registry.Clear();
    }

    public void Dispose()
    {
        Registry.Remove((_world, _name));
        if (_native != IntPtr.Zero)
        {
            Native.UI_DestroyDataModel(_native);
            _native = IntPtr.Zero;
        }
        if (_self.IsAllocated)
        {
            _self.Free();
        }
        _viewModel.Binding = null;
    }

    // ---- type mapping ----

    private static bool TryMapType(Type Type, out UIVarType VarType)
    {
        if (Type.IsEnum) { VarType = UIVarType.Int; return true; }
        if (Type == typeof(bool)) { VarType = UIVarType.Bool; return true; }
        if (Type == typeof(string)) { VarType = UIVarType.String; return true; }
        if (Type == typeof(float)) { VarType = UIVarType.Float; return true; }
        if (Type == typeof(double)) { VarType = UIVarType.Double; return true; }
        if (Type == typeof(int) || Type == typeof(short) || Type == typeof(sbyte) || Type == typeof(byte)
            || Type == typeof(uint) || Type == typeof(ushort) || Type == typeof(long) || Type == typeof(ulong))
        {
            VarType = UIVarType.Int;
            return true;
        }
        VarType = default;
        return false;
    }

    private static bool TryGetItemType(Type Type, out Type ItemType)
    {
        ItemType = typeof(object);
        if (Type == typeof(string))
        {
            return false;
        }
        if (Type.IsGenericType && Type.GetGenericTypeDefinition() == typeof(IEnumerable<>))
        {
            ItemType = Type.GetGenericArguments()[0];
            return true;
        }
        foreach (Type Interface in Type.GetInterfaces())
        {
            if (Interface.IsGenericType && Interface.GetGenericTypeDefinition() == typeof(IEnumerable<>))
            {
                ItemType = Interface.GetGenericArguments()[0];
                return true;
            }
        }
        return false;
    }

    private static bool HasBindMembers(Type Type)
    {
        foreach (PropertyInfo Property in Type.GetProperties(BindingFlags.Public | BindingFlags.Instance))
        {
            if (Property.GetCustomAttribute<BindAttribute>() != null && Property.GetMethod != null)
            {
                return true;
            }
        }
        return false;
    }

    private static ItemMember[] BuildItemMembers(Type ItemType)
    {
        List<ItemMember> Members = new();
        foreach (PropertyInfo Property in ItemType.GetProperties(BindingFlags.Public | BindingFlags.Instance))
        {
            BindAttribute? Attribute = Property.GetCustomAttribute<BindAttribute>();
            if (Attribute == null || Property.GetMethod == null)
            {
                continue;
            }
            Members.Add(new ItemMember(Attribute.Name ?? Property.Name, PropertyAccessor.Getter(Property)));
        }
        return Members.ToArray();
    }

    private static double ToNumber(object? Value, UIVarType Type)
    {
        if (Value == null)
        {
            return 0.0;
        }
        return Type == UIVarType.Bool ? ((bool)Value ? 1.0 : 0.0) : Convert.ToDouble(Value, CultureInfo.InvariantCulture);
    }

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
