using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace LuminaSharp;

// What a type exposes to RML through [Bind], found once per type and shared by every binding of it and by the editor's preview.
internal sealed class UIModelShape
{
    internal sealed class FScalar
    {
        public required string Name;
        public required string MemberName;
        public required Type ValueType;
        public required Lumina.EUIVarType VarType;
        public required Func<object, object?> Get;
        public Action<object, object?>? Set;
    }

    internal sealed class FListItem
    {
        public required string Name;
        public required Func<object, object?> Get;
    }

    internal sealed class FList
    {
        public required string Name;
        public required string MemberName;
        public required Type ItemType;
        public required Func<object, object?> Get;
        public required FListItem[] Items;
    }

    internal sealed class FCommand
    {
        public required string Name;
        public required MethodInfo Method;
        public required ParameterInfo[] Parameters;
    }

    public readonly List<FScalar> Scalars = new();
    public readonly List<FList> Lists = new();
    public readonly List<FCommand> Commands = new();

    // Weak, so a hot reload's old script types are not pinned by the cache.
    private static readonly ConditionalWeakTable<Type, UIModelShape> Cache = new();

    private const BindingFlags Declared = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;

    public static UIModelShape Of(Type Type) => Cache.GetValue(Type, Build);

    public bool IsEmpty => Scalars.Count == 0 && Lists.Count == 0 && Commands.Count == 0;

    // The engine's own base classes carry no [Bind] members, so the walk stops where the user's types end.
    private static IEnumerable<Type> UserHierarchy(Type Type)
    {
        Assembly Engine = typeof(UIModelShape).Assembly;
        for (Type? Current = Type; Current != null && Current != typeof(object) && Current.Assembly != Engine; Current = Current.BaseType)
        {
            yield return Current;
        }
    }

    private static UIModelShape Build(Type Type)
    {
        var Shape = new UIModelShape();
        var Taken = new HashSet<string>(StringComparer.Ordinal);

        foreach (Type Level in UserHierarchy(Type))
        {
            foreach (PropertyInfo Property in Level.GetProperties(Declared))
            {
                if (Property.GetCustomAttribute<BindAttribute>() is not { } Attribute || Property.GetMethod == null)
                {
                    continue;
                }
                Shape.AddValue(Type, Attribute.Name ?? Property.Name, Property.Name, Property.PropertyType,
                    PropertyAccessor.Getter(Property), Property.SetMethod != null ? PropertyAccessor.Setter(Property) : null, Taken);
            }

            foreach (FieldInfo Field in Level.GetFields(Declared))
            {
                if (Field.GetCustomAttribute<BindAttribute>() is not { } Attribute || Field.IsStatic)
                {
                    continue;
                }
                Action<object, object?>? Setter = Field.IsInitOnly ? null : Field.SetValue;
                Shape.AddValue(Type, Attribute.Name ?? Field.Name, Field.Name, Field.FieldType, Field.GetValue, Setter, Taken);
            }

            foreach (MethodInfo Method in Level.GetMethods(Declared))
            {
                BindCommandAttribute? Command = Method.GetCustomAttribute<BindCommandAttribute>();
                BindAttribute? Bind = Method.GetCustomAttribute<BindAttribute>();
                if ((Command == null && Bind == null) || Method.IsStatic || Method.IsGenericMethodDefinition)
                {
                    continue;
                }
                string CommandName = Command?.Name ?? Bind?.Name ?? Method.Name;
                if (!Taken.Add(CommandName))
                {
                    Debug.LogWarning($"[UI] {Type.Name}.{Method.Name}: the name '{CommandName}' is bound twice; this one is skipped.");
                    continue;
                }
                Shape.Commands.Add(new FCommand { Name = CommandName, Method = Method, Parameters = Method.GetParameters() });
            }
        }
        return Shape;
    }

    private void AddValue(Type Owner, string Name, string MemberName, Type ValueType, Func<object, object?> Get,
        Action<object, object?>? Set, HashSet<string> Taken)
    {
        if (!Taken.Add(Name))
        {
            Debug.LogWarning($"[UI] {Owner.Name}.{MemberName}: the name '{Name}' is bound twice; this one is skipped.");
            return;
        }

        if (TryMapType(ValueType, out Lumina.EUIVarType VarType))
        {
            Scalars.Add(new FScalar { Name = Name, MemberName = MemberName, ValueType = ValueType, VarType = VarType, Get = Get, Set = Set });
            return;
        }

        if (TryGetItemType(ValueType, out Type ItemType) && BuildItems(ItemType) is { Length: > 0 } Items)
        {
            Lists.Add(new FList { Name = Name, MemberName = MemberName, ItemType = ItemType, Get = Get, Items = Items });
            return;
        }

        Debug.LogWarning($"[UI] {Owner.Name}.{MemberName}: [Bind] type '{ValueType.Name}' is not bindable. Use a number, bool, enum, "
            + "string, or a collection of a type with [Bind] members.");
    }

    private static FListItem[] BuildItems(Type ItemType)
    {
        var Items = new List<FListItem>();
        foreach (Type Level in UserHierarchy(ItemType))
        {
            foreach (PropertyInfo Property in Level.GetProperties(Declared))
            {
                if (Property.GetCustomAttribute<BindAttribute>() is { } Attribute && Property.GetMethod != null)
                {
                    Items.Add(new FListItem { Name = Attribute.Name ?? Property.Name, Get = PropertyAccessor.Getter(Property) });
                }
            }
            foreach (FieldInfo Field in Level.GetFields(Declared))
            {
                if (Field.GetCustomAttribute<BindAttribute>() is { } Attribute && !Field.IsStatic)
                {
                    Items.Add(new FListItem { Name = Attribute.Name ?? Field.Name, Get = Field.GetValue });
                }
            }
        }
        return Items.ToArray();
    }

    internal static bool TryMapType(Type Type, out Lumina.EUIVarType VarType)
    {
        if (Type.IsEnum) { VarType = Lumina.EUIVarType.Int; return true; }
        if (Type == typeof(bool)) { VarType = Lumina.EUIVarType.Bool; return true; }
        if (Type == typeof(string)) { VarType = Lumina.EUIVarType.String; return true; }
        if (Type == typeof(float)) { VarType = Lumina.EUIVarType.Float; return true; }
        if (Type == typeof(double)) { VarType = Lumina.EUIVarType.Double; return true; }
        if (Type == typeof(int) || Type == typeof(short) || Type == typeof(sbyte) || Type == typeof(byte)
            || Type == typeof(uint) || Type == typeof(ushort) || Type == typeof(long) || Type == typeof(ulong))
        {
            VarType = Lumina.EUIVarType.Int;
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

    // Changes when the collection is replaced, resized or reordered. An item edited in place needs NotifyChanged.
    internal static long Signature(object? Collection)
    {
        if (Collection is not IEnumerable Items)
        {
            return 0;
        }
        long Hash = RuntimeHelpers.GetHashCode(Collection);
        foreach (object? Item in Items)
        {
            // A boxed struct is a fresh object each time, so a value item hashes by value instead.
            int ItemHash = Item == null ? 7 : (Item.GetType().IsValueType ? Item.GetHashCode() : RuntimeHelpers.GetHashCode(Item));
            Hash = Hash * 31 + ItemHash;
        }
        return Hash;
    }
}
