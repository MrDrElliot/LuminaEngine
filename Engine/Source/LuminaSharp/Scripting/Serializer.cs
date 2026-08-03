using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace LuminaSharp;

/// <summary>
/// The recursive blob codec bridging managed script reflection and the native script schema/value
/// model (Lumina::Scripting::FScriptExportSchema / FScriptPropertyValue, which is already nesting-complete).
/// One self-describing value encoding (a leading kind byte) is used both for the schema's default
/// values and for the per-instance overrides, so nested structs and arrays round-trip uniformly. The
/// managed side WRITES the schema (+ defaults) and READS overrides onto a live instance; the native
/// side does the mirror.
/// </summary>
internal static class Serializer
{
    // ---- Schema (managed -> native): types + meta + default values, recursive ----

    public static byte[] WriteSchema(TypeDescription Description)
    {
        object? Defaults = Description.Create();

        using var Stream = new MemoryStream();
        using var Writer = new BinaryWriter(Stream, Encoding.UTF8, leaveOpen: true);

        Writer.Write(Description.Properties.Count);
        foreach (ScriptProperty Property in Description.Properties)
        {
            WriteString(Writer, Property.Name);
            WriteAliases(Writer, Property.Aliases);
            WriteMeta(Writer, Property.Meta);
            Writer.Write((byte)(Property.SkipHotReload ? 1 : 0));
            WriteType(Writer, Property.Type);
            WriteValue(Writer, Property.Type, Defaults != null ? Property.Get(Defaults) : null);
        }

        Writer.Flush();
        return Stream.ToArray();
    }

    // ---- Buttons (managed -> native): [Button] method name + label + tooltip, flat ----

    public static byte[] WriteButtons(TypeDescription Description)
    {
        using var Stream = new MemoryStream();
        using var Writer = new BinaryWriter(Stream, Encoding.UTF8, leaveOpen: true);

        Writer.Write(Description.Buttons.Count);
        foreach (ScriptButton Button in Description.Buttons)
        {
            WriteString(Writer, Button.Method);
            WriteString(Writer, Button.Label);
            WriteString(Writer, Button.Tooltip);
        }

        Writer.Flush();
        return Stream.ToArray();
    }

    private static void WriteMeta(BinaryWriter Writer, PropertyAttribute? Meta)
    {
        WriteString(Writer, Meta?.Category ?? "");
        WriteString(Writer, Meta?.Tooltip ?? "");
        WriteString(Writer, Meta?.Units ?? "");

        bool bHasMin = Meta?.HasMin ?? false;
        Writer.Write((byte)(bHasMin ? 1 : 0));
        if (bHasMin)
        {
            Writer.Write((double)Meta!.Min);
        }

        bool bHasMax = Meta?.HasMax ?? false;
        Writer.Write((byte)(bHasMax ? 1 : 0));
        if (bHasMax)
        {
            Writer.Write((double)Meta!.Max);
        }

        Writer.Write((byte)((Meta?.Color ?? false) ? 1 : 0));
    }

    // Type descriptor; parsed in lockstep by the native ReadType. The kind is the shared reflected taxonomy
    // (EPropertyType); the entity byte rides alongside because an entity is a plain UInt32 with no kind of its
    // own. A Struct always writes its NativeName (empty for a C#-defined script struct) so the reader tells the
    // two apart from one uniform shape.
    private static void WriteType(BinaryWriter Writer, ScriptType Type)
    {
        Writer.Write((byte)Type.Kind);
        Writer.Write((byte)(Type.IsEntity ? 1 : 0));
        Writer.Write((byte)(Type.IsInputAction ? 1 : 0));
        switch (Type.Kind)
        {
            case EPropertyType.Enum:
            {
                WriteString(Writer, Type.EnumName ?? "");
                Writer.Write((byte)Type.EnumUnderlying);
                IReadOnlyList<EnumEntry> Entries = Type.EnumEntries ?? Array.Empty<EnumEntry>();
                Writer.Write(Entries.Count);
                foreach (EnumEntry Entry in Entries)
                {
                    WriteString(Writer, Entry.Name);
                    Writer.Write(Entry.Value);
                }
                break;
            }
            case EPropertyType.Struct:
            {
                WriteString(Writer, Type.NativeName ?? "");
                WriteFields(Writer, Type.Fields, Type.Clr);
                break;
            }
            case EPropertyType.SoftObject:
            {
                WriteString(Writer, Type.TargetClass ?? "");
                break;
            }
            case EPropertyType.Vector:
            {
                WriteType(Writer, Type.Element ?? new ScriptType());
                break;
            }
            case EPropertyType.Map:
            {
                WriteType(Writer, Type.KeyType ?? new ScriptType());
                WriteType(Writer, Type.ValueType ?? new ScriptType());
                break;
            }
            case EPropertyType.InstancedStruct:
            {
                WriteString(Writer, Type.BaseName ?? "");
                IReadOnlyList<ScriptInstanceCandidate> Candidates = Type.Candidates ?? Array.Empty<ScriptInstanceCandidate>();
                Writer.Write(Candidates.Count);
                foreach (ScriptInstanceCandidate Candidate in Candidates)
                {
                    WriteString(Writer, Candidate.TypeName);
                    WriteFields(Writer, Candidate.Fields, Candidate.Clr);
                }
                break;
            }
        }
    }

    // Nested fields carry their default alongside the type, exactly as the top-level schema does. Without
    // it a minted sub-struct or instanced candidate is only default-constructed on the native side, so every
    // C# field initializer is lost and the inspector shows zeroes.
    private static void WriteFields(BinaryWriter Writer, IReadOnlyList<ScriptProperty>? Fields, Type? Owner)
    {
        IReadOnlyList<ScriptProperty> List = Fields ?? Array.Empty<ScriptProperty>();
        object? Defaults = TryCreateDefaults(Owner);

        Writer.Write(List.Count);
        foreach (ScriptProperty Field in List)
        {
            WriteString(Writer, Field.Name);
            WriteAliases(Writer, Field.Aliases);
            WriteMeta(Writer, Field.Meta);
            WriteType(Writer, Field.Type);
            WriteValue(Writer, Field.Type, Defaults != null ? Field.Get(Defaults) : null);
        }
    }

    // A throwaway instance purely to read field initializers off. Instanced candidates are filtered to
    // default-constructible types, and script structs are value types or classes with a parameterless ctor,
    // so this normally succeeds; a type that resists it just falls back to zeroes as before.
    private static object? TryCreateDefaults(Type? Owner)
    {
        if (Owner == null)
        {
            return null;
        }
        try
        {
            return Activator.CreateInstance(Owner);
        }
        catch (Exception Ex)
        {
            Native.Log(ELogLevel.Warn,
                $"Serializer: could not instantiate '{Owner.Name}' to read its defaults ({Ex.GetType().Name}); "
                + "its fields will start at zero.");
            return null;
        }
    }

    private static void WriteAliases(BinaryWriter Writer, IReadOnlyList<string>? Aliases)
    {
        IReadOnlyList<string> List = Aliases ?? Array.Empty<string>();
        Writer.Write(List.Count);
        foreach (string Alias in List)
        {
            WriteString(Writer, Alias);
        }
    }

    // Coarse self-describing value with a leading EScriptValueKind byte.
    private static void WriteValue(BinaryWriter Writer, ScriptType Type, object? Value)
    {
        EScriptValueKind Kind = Type.ValueKind;
        Writer.Write((byte)Kind);
        switch (Kind)
        {
            case EScriptValueKind.Bool:
            {
                Writer.Write((byte)(Value is bool Bool && Bool ? 1 : 0));
                break;
            }
            case EScriptValueKind.Int:
            {
                Writer.Write(EncodeInt(Type, Value));
                break;
            }
            case EScriptValueKind.Double:
            {
                Writer.Write(Value != null ? Convert.ToDouble(Value) : 0.0);
                break;
            }
            case EScriptValueKind.String:
            {
                WriteString(Writer, EncodeString(Type, Value));
                break;
            }
            case EScriptValueKind.Nested:
            {
                WriteNested(Writer, Type, Value);
                break;
            }
            case EScriptValueKind.Array:
            {
                WriteArray(Writer, Type, Value);
                break;
            }
            case EScriptValueKind.Map:
            {
                WriteMap(Writer, Type, Value);
                break;
            }
            case EScriptValueKind.Instance:
            {
                WriteInstance(Writer, Type, Value);
                break;
            }
        }
    }

    // Writes the chosen type name, then (when non-empty) a field count and each field's name and value.
    // An unmatched or null instance writes an empty name. The candidate is found by runtime CLR type.
    private static void WriteInstance(BinaryWriter Writer, ScriptType Type, object? Value)
    {
        IReadOnlyList<ScriptInstanceCandidate> Candidates = Type.Candidates ?? Array.Empty<ScriptInstanceCandidate>();
        ScriptInstanceCandidate? Chosen = null;
        if (Value != null)
        {
            Type Runtime = Value.GetType();
            foreach (ScriptInstanceCandidate Candidate in Candidates)
            {
                if (Candidate.Clr == Runtime)
                {
                    Chosen = Candidate;
                    break;
                }
            }
        }

        if (Chosen == null)
        {
            WriteString(Writer, "");
            return;
        }

        WriteString(Writer, Chosen.TypeName);
        Writer.Write(Chosen.Fields.Count);
        foreach (ScriptProperty Field in Chosen.Fields)
        {
            WriteString(Writer, Field.Name);
            WriteValue(Writer, Field.Type, Value != null ? Field.Get(Value) : null);
        }
    }

    private static long EncodeInt(ScriptType Type, object? Value)
    {
        if (Value == null)
        {
            return 0L;
        }
        if (Type.IsEntity)
        {
            return Value is Entity Ent ? Ent.Id : 0L;
        }
        // Bit-preserving for unsigned values: Convert.ToInt64 throws OverflowException on a ulong (or a
        // ulong-backed enum) whose high bit is set, which would abort the whole schema/value blob and
        // silently drop every property on the type. Reinterpret the bits instead; DecodeInt casts back.
        Type Underlying = Value.GetType();
        if (Underlying.IsEnum)
        {
            Underlying = Enum.GetUnderlyingType(Underlying);
        }
        if (Underlying == typeof(ulong))
        {
            return unchecked((long)Convert.ToUInt64(Value));
        }
        return Convert.ToInt64(Value);
    }

    private static string EncodeString(ScriptType Type, object? Value)
    {
        if (Type.Kind == EPropertyType.SoftObject)
        {
            return Value is IAssetRef Reference ? Reference.GetPath() : "";
        }
        if (Type.IsInputAction)
        {
            return Value is InputBinding Binding ? Binding.Name : "";
        }
        return Value as string ?? "";
    }

    private static void WriteArray(BinaryWriter Writer, ScriptType Type, object? Value)
    {
        ScriptType Element = Type.Element ?? new ScriptType();
        if (Value is IEnumerable Enumerable && Type.Element != null)
        {
            var Items = new List<object?>();
            foreach (object? Item in Enumerable)
            {
                Items.Add(Item);
            }
            Writer.Write(Items.Count);
            foreach (object? Item in Items)
            {
                WriteValue(Writer, Element, Item);
            }
        }
        else
        {
            Writer.Write(0);
        }
    }

    // Count followed by that many [key value, value value] pairs, each via the recursive value codec.
    private static void WriteMap(BinaryWriter Writer, ScriptType Type, object? Value)
    {
        ScriptType Key = Type.KeyType ?? new ScriptType();
        ScriptType Val = Type.ValueType ?? new ScriptType();
        if (Value is IDictionary Dict && Type.KeyType != null && Type.ValueType != null)
        {
            Writer.Write(Dict.Count);
            IDictionaryEnumerator Enumerator = Dict.GetEnumerator();
            while (Enumerator.MoveNext())
            {
                WriteValue(Writer, Key, Enumerator.Key);
                WriteValue(Writer, Val, Enumerator.Value);
            }
        }
        else
        {
            Writer.Write(0);
        }
    }

    private static void WriteNested(BinaryWriter Writer, ScriptType Type, object? Value)
    {
        IReadOnlyList<ScriptProperty> Fields = Type.Fields ?? Array.Empty<ScriptProperty>();
        Writer.Write(Fields.Count);
        foreach (ScriptProperty Field in Fields)
        {
            WriteString(Writer, Field.Name);
            WriteValue(Writer, Field.Type, Value != null ? Field.Get(Value) : null);
        }
    }

    private static void WriteString(BinaryWriter Writer, string Value)
    {
        byte[] Bytes = Encoding.UTF8.GetBytes(Value);
        Writer.Write(Bytes.Length);
        Writer.Write(Bytes);
    }

    // ---- Overrides (native -> managed): apply a self-describing value blob onto a live instance ----

    public static unsafe void ApplyValues(object Instance, IReadOnlyList<ScriptProperty> Properties, byte* Blob, int Length)
    {
        var Reader = new FBlobReader(new ReadOnlySpan<byte>(Blob, Length));
        int Count = Reader.ReadInt32();
        for (int Index = 0; Index < Count; Index++)
        {
            string Name = Reader.ReadString();
            ScriptProperty? Property = FindProperty(Properties, Name);
            if (Property == null)
            {
                SkipValue(ref Reader);
                continue;
            }

            if (ReadValue(ref Reader, Property.Type, out object? Value))
            {
                Assign(Property, Instance, Value);
            }
        }
    }

    /// <summary>
    /// Applies a decoded value to one member. An input binding is renamed in place rather than replaced:
    /// the live object owns the script's event subscriptions, so swapping it in would silently drop every
    /// handler the script attached in OnReady.
    /// </summary>
    private static void Assign(ScriptProperty Property, object Instance, object? Value)
    {
        if (Property.Type.IsInputAction
            && Value is InputBinding Incoming
            && Property.Get(Instance) is InputBinding Existing)
        {
            Existing.Name = Incoming.Name;
            return;
        }
        Property.Set(Instance, Value);
    }

    private static ScriptProperty? FindProperty(IReadOnlyList<ScriptProperty> Properties, string Name)
    {
        foreach (ScriptProperty Property in Properties)
        {
            // Case-insensitive so a native struct member matches its differently-cased C# mirror.
            if (string.Equals(Property.Name, Name, StringComparison.OrdinalIgnoreCase))
            {
                return Property;
            }
        }
        return null;
    }

    /// <summary>
    /// Reads one self-describing value. If the encoded kind doesn't match the target shape (schema
    /// drift) the bytes are consumed and false is returned so the field keeps its default.
    /// </summary>
    private static bool ReadValue(ref FBlobReader Reader, ScriptType Type, out object? Value)
    {
        Value = null;

        var Kind = (EScriptValueKind)Reader.ReadByte();
        if (Kind != Type.ValueKind)
        {
            SkipBody(ref Reader, Kind);
            return false;
        }

        switch (Kind)
        {
            case EScriptValueKind.Bool:
            {
                Value = Reader.ReadByte() != 0;
                return true;
            }
            case EScriptValueKind.Int:
            {
                Value = DecodeInt(Reader.ReadInt64(), Type);
                return true;
            }
            case EScriptValueKind.Double:
            {
                // Box the EXACT field type: a `? (float) : (double)` ternary re-widens both branches to
                // double, so a float field would get a boxed double and FieldInfo.SetValue throws.
                double Number = Reader.ReadDouble();
                if (Type.Clr == typeof(float))
                {
                    Value = (float)Number;
                }
                else
                {
                    Value = Number;
                }
                return true;
            }
            case EScriptValueKind.String:
            {
                return DecodeString(ref Reader, Type, out Value);
            }
            case EScriptValueKind.Array:
            {
                return ReadArray(ref Reader, Type, out Value);
            }
            case EScriptValueKind.Map:
            {
                return ReadMap(ref Reader, Type, out Value);
            }
            case EScriptValueKind.Nested:
            {
                return ReadNested(ref Reader, Type, out Value);
            }
            case EScriptValueKind.Instance:
            {
                return ReadInstance(ref Reader, Type, out Value);
            }
            default:
            {
                return false;
            }
        }
    }

    private static bool ReadInstance(ref FBlobReader Reader, ScriptType Type, out object? Value)
    {
        Value = null;

        string TypeName = Reader.ReadString();
        if (string.IsNullOrEmpty(TypeName))
        {
            return true; // a null instance is a valid value
        }

        ScriptInstanceCandidate? Chosen = null;
        IReadOnlyList<ScriptInstanceCandidate> Candidates = Type.Candidates ?? Array.Empty<ScriptInstanceCandidate>();
        foreach (ScriptInstanceCandidate Candidate in Candidates)
        {
            if (Candidate.TypeName == TypeName)
            {
                Chosen = Candidate;
                break;
            }
        }

        int Count = Reader.ReadInt32();
        if (Chosen == null)
        {
            // Unknown type (removed/renamed): consume the body, leave the field at its default.
            for (int Index = 0; Index < Count; Index++)
            {
                Reader.Skip(Reader.ReadInt32()); // field name
                SkipValue(ref Reader);
            }
            return false;
        }

        object? Box = Activator.CreateInstance(Chosen.Clr);
        for (int Index = 0; Index < Count; Index++)
        {
            string Name = Reader.ReadString();
            ScriptProperty? Field = FindProperty(Chosen.Fields, Name);
            if (Field == null)
            {
                SkipValue(ref Reader);
                continue;
            }
            if (Box != null && ReadValue(ref Reader, Field.Type, out object? FieldValue))
            {
                Assign(Field, Box, FieldValue);
            }
        }

        Value = Box;
        return Box != null;
    }

    private static bool DecodeString(ref FBlobReader Reader, ScriptType Type, out object? Value)
    {
        string Text = Reader.ReadString();
        if (Type.IsInputAction)
        {
            // A fresh binding; Assign hands the name to the existing one instead when there is one, so
            // event subscriptions made in OnReady survive a value push from the editor.
            object? Box = Activator.CreateInstance(Type.Clr);
            if (Box is InputBinding Binding)
            {
                Binding.Name = Text;
            }
            Value = Box;
            return Box != null;
        }
        if (Type.Kind == EPropertyType.SoftObject)
        {
            object? Box = Activator.CreateInstance(Type.Clr);
            if (Box is IAssetRef Reference)
            {
                Reference.SetFromPath(Text);
            }
            Value = Box;
            return Box != null;
        }
        Value = Text;
        return true;
    }

    private static bool ReadArray(ref FBlobReader Reader, ScriptType Type, out object? Value)
    {
        Value = null;
        int Count = Reader.ReadInt32();
        ScriptType Element = Type.Element ?? new ScriptType();
        Type ElementClr = Element.Clr;

        var Items = new List<object?>(Count);
        for (int Index = 0; Index < Count; Index++)
        {
            if (ReadValue(ref Reader, Element, out object? Item))
            {
                Items.Add(Item);
            }
        }

        if (Type.Clr.IsArray)
        {
            Array Result = Array.CreateInstance(ElementClr, Items.Count);
            for (int Index = 0; Index < Items.Count; Index++)
            {
                Result.SetValue(Items[Index], Index);
            }
            Value = Result;
            return true;
        }

        // List<T>
        if (Activator.CreateInstance(Type.Clr) is IList List)
        {
            foreach (object? Item in Items)
            {
                List.Add(Item);
            }
            Value = List;
            return true;
        }

        return false;
    }

    private static bool ReadMap(ref FBlobReader Reader, ScriptType Type, out object? Value)
    {
        Value = null;
        int Count = Reader.ReadInt32();
        ScriptType Key = Type.KeyType ?? new ScriptType();
        ScriptType Val = Type.ValueType ?? new ScriptType();

        // The concrete Dictionary<K,V> reached through IDictionary so key/value box/unbox to the exact CLR types.
        if (Activator.CreateInstance(Type.Clr) is not IDictionary Dict)
        {
            for (int Index = 0; Index < Count; Index++)
            {
                SkipValue(ref Reader); // key
                SkipValue(ref Reader); // value
            }
            return false;
        }

        for (int Index = 0; Index < Count; Index++)
        {
            bool HasKey = ReadValue(ref Reader, Key, out object? KeyValue);
            bool HasValue = ReadValue(ref Reader, Val, out object? ValueValue);
            if (HasKey && KeyValue != null && HasValue)
            {
                Dict[KeyValue] = ValueValue;
            }
        }

        Value = Dict;
        return true;
    }

    private static bool ReadNested(ref FBlobReader Reader, ScriptType Type, out object? Value)
    {
        object? Box = Activator.CreateInstance(Type.Clr);
        IReadOnlyList<ScriptProperty> Fields = Type.Fields ?? Array.Empty<ScriptProperty>();

        int Count = Reader.ReadInt32();
        for (int Index = 0; Index < Count; Index++)
        {
            string Name = Reader.ReadString();
            ScriptProperty? Field = FindProperty(Fields, Name);
            if (Field == null)
            {
                SkipValue(ref Reader);
                continue;
            }
            if (Box != null && ReadValue(ref Reader, Field.Type, out object? FieldValue))
            {
                Assign(Field, Box, FieldValue);
            }
        }

        Value = Box;
        return Box != null;
    }

    private static object DecodeInt(long Value, ScriptType Type)
    {
        if (Type.IsEntity)
        {
            return new Entity(unchecked((uint)Value));
        }
        Type Target = Type.Clr;
        if (Target.IsEnum)
        {
            return Enum.ToObject(Target, Value);
        }
        if (Target == typeof(ulong))
        {
            return (ulong)Value;
        }
        if (Target == typeof(long))
        {
            return Value;
        }
        return Convert.ChangeType(Value, Target);
    }

    private static void SkipValue(ref FBlobReader Reader)
    {
        var Kind = (EScriptValueKind)Reader.ReadByte();
        SkipBody(ref Reader, Kind);
    }

    private static void SkipBody(ref FBlobReader Reader, EScriptValueKind Kind)
    {
        switch (Kind)
        {
            case EScriptValueKind.Bool:
            {
                Reader.Skip(1);
                break;
            }
            case EScriptValueKind.Int:
            {
                Reader.Skip(8);
                break;
            }
            case EScriptValueKind.Double:
            {
                Reader.Skip(8);
                break;
            }
            case EScriptValueKind.String:
            {
                Reader.Skip(Reader.ReadInt32());
                break;
            }
            case EScriptValueKind.Array:
            {
                int Count = Reader.ReadInt32();
                for (int Index = 0; Index < Count; Index++)
                {
                    SkipValue(ref Reader);
                }
                break;
            }
            case EScriptValueKind.Map:
            {
                int Count = Reader.ReadInt32();
                for (int Index = 0; Index < Count; Index++)
                {
                    SkipValue(ref Reader); // key
                    SkipValue(ref Reader); // value
                }
                break;
            }
            case EScriptValueKind.Nested:
            {
                int Count = Reader.ReadInt32();
                for (int Index = 0; Index < Count; Index++)
                {
                    Reader.Skip(Reader.ReadInt32()); // field name
                    SkipValue(ref Reader);
                }
                break;
            }
            case EScriptValueKind.Instance:
            {
                string Name = Reader.ReadString();
                if (!string.IsNullOrEmpty(Name))
                {
                    int Count = Reader.ReadInt32();
                    for (int Index = 0; Index < Count; Index++)
                    {
                        Reader.Skip(Reader.ReadInt32()); // field name
                        SkipValue(ref Reader);
                    }
                }
                break;
            }
        }
    }
}

/// <summary>Little-endian cursor over a native value blob.</summary>
internal ref struct FBlobReader
{
    private ReadOnlySpan<byte> Span;
    private int Position;

    public FBlobReader(ReadOnlySpan<byte> Span)
    {
        this.Span = Span;
        Position = 0;
    }

    public byte ReadByte()
    {
        if (Position >= Span.Length)
        {
            return 0;
        }
        return Span[Position++];
    }

    public int ReadInt32()
    {
        if (Position + 4 > Span.Length)
        {
            Position = Span.Length;
            return 0;
        }
        int Value = BitConverter.ToInt32(Span.Slice(Position, 4));
        Position += 4;
        return Value;
    }

    public long ReadInt64()
    {
        if (Position + 8 > Span.Length)
        {
            Position = Span.Length;
            return 0;
        }
        long Value = BitConverter.ToInt64(Span.Slice(Position, 8));
        Position += 8;
        return Value;
    }

    public double ReadDouble()
    {
        if (Position + 8 > Span.Length)
        {
            Position = Span.Length;
            return 0.0;
        }
        double Value = BitConverter.ToDouble(Span.Slice(Position, 8));
        Position += 8;
        return Value;
    }

    public string ReadString()
    {
        int Length = ReadInt32();
        if (Length <= 0 || Position + Length > Span.Length)
        {
            return string.Empty;
        }
        string Value = Encoding.UTF8.GetString(Span.Slice(Position, Length));
        Position += Length;
        return Value;
    }

    public void Skip(int Bytes)
    {
        if (Bytes < 0)
        {
            return;
        }
        Position = Math.Min(Span.Length, Position + Bytes);
    }
}
