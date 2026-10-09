using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// The document a UIScript shows when the Document property on the entity is left empty, the C# spelling of REFLECT(UIDocument = ...).
[AttributeUsage(AttributeTargets.Class, Inherited = true, AllowMultiple = false)]
public sealed class UIDocumentAttribute : Attribute
{
    public UIDocumentAttribute(string Path)
    {
        this.Path = Path;
    }

    public string Path { get; }

    // The class's starting value for the Interactive property, true for a menu that takes clicks.
    public bool Interactive { get; set; }

    // The class's starting value for the ShowOnStart property.
    public bool ShowOnStart { get; set; } = true;
}

// Fills a UIElement field or property with the element of this id, or of the member's own name, each time the document loads.
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = false)]
public sealed class ElementAttribute : Attribute
{
    public ElementAttribute(string? Id = null)
    {
        this.Id = Id;
    }

    public string? Id { get; }
}

// An entity script that is its own screen UI, the C# twin of the C++ CUIScript. The engine shows Document with this script's [Bind] members as the data model.
public abstract unsafe class UIScript : EntityScript
{
    [Property(Category = "UI", Tooltip = "The .rml this script shows. Empty uses the class's [UIDocument].", AssetType = "rml")]
    public string Document
    {
        get => HasNativeStorage ? NativeMarshal.ReadString((nint)Handle + UIScriptLayout.Offset(GetType(), nameof(Document))) : _unboundDocument;
        set
        {
            if (HasNativeStorage)
            {
                Native.PropSetString(Handle, UIScriptLayout.Token(GetType(), nameof(Document)), value ?? "");
            }
            else
            {
                _unboundDocument = value ?? "";
            }
        }
    }

    [Property(Category = "UI", Tooltip = "Show the document as soon as it loads. Off keeps it hidden until Show().")]
    public bool ShowOnStart
    {
        get => ReadFlag(nameof(ShowOnStart), _unboundShowOnStart);
        set => WriteFlag(nameof(ShowOnStart), value, ref _unboundShowOnStart);
    }

    [Property(Category = "UI", Tooltip = "Free the cursor and let the document take clicks while it is shown, for menus. Off leaves gameplay input alone, for a HUD.")]
    public bool Interactive
    {
        get => ReadFlag(nameof(Interactive), _unboundInteractive);
        set => WriteFlag(nameof(Interactive), value, ref _unboundInteractive);
    }

    private string _unboundDocument = "";
    private bool _unboundShowOnStart;
    private bool _unboundInteractive;

    protected UIScript()
    {
        UIDocumentAttribute? Declared = GetType().GetCustomAttribute<UIDocumentAttribute>(inherit: true);
        _unboundShowOnStart = Declared?.ShowOnStart ?? true;
        _unboundInteractive = Declared?.Interactive ?? false;
    }

    // The loaded document, invalid while the script has none.
    public UIDocument View => new((ulong)NativeObjectMarshal.ToHandle(World), Lumina.CUILibrary.GetScriptDocument(this));

    public bool IsShown => Lumina.CUILibrary.IsScriptUIShown(this);

    // The name the document's data-model attribute uses, from [DataModel] or the class name.
    public string ModelName => DataModelAttribute.NameOf(GetType());

    public void Show() => Lumina.CUILibrary.ShowScriptUI(this);

    public void Hide() => Lumina.CUILibrary.HideScriptUI(this);

    public void Toggle()
    {
        if (IsShown)
        {
            Hide();
        }
        else
        {
            Show();
        }
    }

    // Runs after every load, including a reload from an edited .rml, once [Element] members point at the new elements.
    protected virtual void OnDocumentLoaded()
    {
    }

    // The default the class default object takes, so an entity's inspector starts from the class's choices.
    protected override void __ApplyScriptDefaults()
    {
        base.__ApplyScriptDefaults();
        UIDocumentAttribute? Declared = GetType().GetCustomAttribute<UIDocumentAttribute>(inherit: true);
        ShowOnStart = Declared?.ShowOnStart ?? true;
        Interactive = Declared?.Interactive ?? false;
    }

    internal void DocumentLoaded()
    {
        UIDocument Loaded = View;
        foreach (UIScriptLayout.FElementSlot Slot in UIScriptLayout.ElementSlots(GetType()))
        {
            UIElement Element = Loaded.GetElementById(Slot.Id);
            if (!Element.IsValid)
            {
                Debug.LogWarning($"[UI] {GetType().Name}.{Slot.MemberName} found no element with id '{Slot.Id}'.");
            }
            Slot.Set(this, Element);
        }

        try
        {
            using var Scope = Engine.Push(World, Entity, this);
            OnDocumentLoaded();
        }
        catch (Exception Exception)
        {
            NativeBindings.ScriptEventException(this, nameof(OnDocumentLoaded), Exception);
        }
    }

    private bool ReadFlag(string Name, bool Unbound)
    {
        return HasNativeStorage ? Unsafe.ReadUnaligned<bool>((void*)((nint)Handle + UIScriptLayout.Offset(GetType(), Name))) : Unbound;
    }

    private void WriteFlag(string Name, bool Value, ref bool Unbound)
    {
        if (HasNativeStorage)
        {
            Unsafe.WriteUnaligned((void*)((nint)Handle + UIScriptLayout.Offset(GetType(), Name)), Value);
        }
        else
        {
            Unbound = Value;
        }
    }

    // Native calls this when a UIScript's document loads, since [Element] fills managed members.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void UIScriptDocumentLoaded(IntPtr Handle)
    {
        try
        {
            if (GCHandle.FromIntPtr(Handle).Target is UIScript Script)
            {
                Script.DocumentLoaded();
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }
}

// Where a UIScript's own properties sit in each subclass's native block, and which members [Element] fills.
internal static class UIScriptLayout
{
    internal sealed class FElementSlot
    {
        public required string Id;
        public required string MemberName;
        public required Action<object, object?> Set;
    }

    private sealed class FTypeInfo
    {
        public readonly Dictionary<string, int> Offsets = new(StringComparer.Ordinal);
        public readonly Dictionary<string, IntPtr> Tokens = new(StringComparer.Ordinal);
        public FElementSlot[]? Elements;
    }

    // Weak, so a hot reload's old script types are not pinned.
    private static readonly ConditionalWeakTable<Type, FTypeInfo> Types = new();

    public static int Offset(Type Type, string Name)
    {
        FTypeInfo Info = Types.GetOrCreateValue(Type);
        lock (Info)
        {
            if (!Info.Offsets.TryGetValue(Name, out int Value))
            {
                Value = NativeBindings.PropertyOffset(NativeTypeName.Of(Type), Name);
                Info.Offsets[Name] = Value;
            }
            return Value;
        }
    }

    public static IntPtr Token(Type Type, string Name)
    {
        FTypeInfo Info = Types.GetOrCreateValue(Type);
        lock (Info)
        {
            if (!Info.Tokens.TryGetValue(Name, out IntPtr Value))
            {
                Value = NativeBindings.FindProperty(NativeTypeName.Of(Type), Name);
                if (Value == IntPtr.Zero)
                {
                    return Value;
                }
                Info.Tokens[Name] = Value;
            }
            return Value;
        }
    }

    public static FElementSlot[] ElementSlots(Type Type)
    {
        FTypeInfo Info = Types.GetOrCreateValue(Type);
        lock (Info)
        {
            if (Info.Elements != null)
            {
                return Info.Elements;
            }

            var Slots = new List<FElementSlot>();
            const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
            for (Type? Current = Type; Current != null && Current != typeof(UIScript); Current = Current.BaseType)
            {
                foreach (FieldInfo Field in Current.GetFields(Flags))
                {
                    if (Field.GetCustomAttribute<ElementAttribute>() is { } Attribute && Field.FieldType == typeof(UIElement))
                    {
                        Slots.Add(new FElementSlot { Id = Attribute.Id ?? Field.Name, MemberName = Field.Name, Set = Field.SetValue });
                    }
                }
                foreach (PropertyInfo Property in Current.GetProperties(Flags))
                {
                    if (Property.GetCustomAttribute<ElementAttribute>() is { } Attribute && Property.PropertyType == typeof(UIElement)
                        && PropertyAccessor.Setter(Property) is { } Setter)
                    {
                        Slots.Add(new FElementSlot { Id = Attribute.Id ?? Property.Name, MemberName = Property.Name, Set = Setter });
                    }
                }
            }
            Info.Elements = Slots.ToArray();
            return Info.Elements;
        }
    }
}
