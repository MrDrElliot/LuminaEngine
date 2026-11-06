using System;
using System.Linq;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace LuminaSharp;

// Writes a type's schema, its fields' types and metadata and its functions, which the native ScriptSchemaCodec reads.
internal static class Serializer
{
    // 'LSCH', mirroring Lumina::Scripting::kScriptSchemaMagic.
    private const uint SchemaMagic = 0x4843534Cu;

    // Mirrors Lumina::Scripting::kScriptSchemaVersion, bumped only when an existing field changes meaning, type or position.
    private const ushort SchemaVersion = 3;

    // Mirrors Lumina::Scripting::EScriptSchemaRecord.
    private enum ERecord : byte
    {
        Field = 0,
        Meta,
        Type,
        Function,
    }

    // A tagged, length-framed record whose length is backfilled on dispose, so the native reader can skip what it does not understand.
    private readonly struct Record : IDisposable
    {
        private readonly BinaryWriter Writer;
        private readonly long LengthPosition;

        public Record(BinaryWriter InWriter, ERecord Tag)
        {
            Writer = InWriter;
            Writer.Write((byte)Tag);
            LengthPosition = Writer.BaseStream.Position;
            Writer.Write(0u);
        }

        public void Dispose()
        {
            long End = Writer.BaseStream.Position;
            Writer.BaseStream.Position = LengthPosition;
            Writer.Write((uint)(End - LengthPosition - sizeof(uint)));
            Writer.BaseStream.Position = End;
        }
    }

    // MintedParent's members already live in its own minted class, so only what derives below it is written.
    internal static List<ScriptProperty> PropertiesBelow(TypeDescription Description, Type? MintedParent)
    {
        return MintedParent == null
            ? new List<ScriptProperty>(Description.Properties)
            : Description.Properties.Where(Property => Property.DeclaringType == null || !Property.DeclaringType.IsAssignableFrom(MintedParent)).ToList();
    }

    public static byte[] WriteSchema(TypeDescription Description, Type? MintedParent = null)
    {
        List<ScriptProperty> Properties = PropertiesBelow(Description, MintedParent);

        using var Stream = new MemoryStream();
        using var Writer = new BinaryWriter(Stream, Encoding.UTF8, leaveOpen: true);

        Writer.Write(SchemaMagic);
        Writer.Write(SchemaVersion);

        Writer.Write(Properties.Count);
        foreach (ScriptProperty Property in Properties)
        {
            WriteField(Writer, Property, bTopLevel: true);
        }

        // After the fields, so an older buffer without functions leaves the list empty.
        Writer.Write(Description.Functions.Count);
        foreach (ScriptFunction Function in Description.Functions)
        {
            using var Frame = new Record(Writer, ERecord.Function);
            WriteString(Writer, Function.Name);
            Writer.Write(Function.ReturnIndex);
            Writer.Write(Function.Params.Count);
            foreach (ScriptProperty Parameter in Function.Params)
            {
                WriteField(Writer, Parameter, bTopLevel: false);
            }
            Writer.Write(Function.Flags);
            Writer.Write(Function.Meta.Count);
            foreach (KeyValuePair<string, string> Pair in Function.Meta)
            {
                WriteString(Writer, Pair.Key);
                WriteString(Writer, Pair.Value);
            }
        }

        Writer.Flush();
        return Stream.ToArray();
    }

    // The one writer for a field and a function parameter alike, mirroring the native ReadField.
    private static void WriteField(BinaryWriter Writer, ScriptProperty Property, bool bTopLevel)
    {
        using var Frame = new Record(Writer, ERecord.Field);

        WriteString(Writer, Property.Name);
        WriteAliases(Writer, Property.Aliases);
        WriteMeta(Writer, Property.Meta, Property.Hidden, Property.ParamFlags, Property.ExtraFlags, Property.NetRate, Property.ExtraMeta);

        // Only a top-level field has a hot-reload identity, so only it carries the byte.
        if (bTopLevel)
        {
            Writer.Write((byte)(Property.SkipHotReload ? 1 : 0));
        }

        WriteType(Writer, Property.Type);
    }

    private static void WriteMeta(BinaryWriter Writer, PropertyAttribute? Meta, bool bHidden = false,
        EScriptParamFlags ParamFlags = EScriptParamFlags.None, EPropertyFlags ExtraFlags = EPropertyFlags.None, float NetRate = 0.0f,
        IReadOnlyList<KeyValuePair<string, string>>? ExtraMeta = null)
    {
        using var Frame = new Record(Writer, ERecord.Meta);

        WriteString(Writer, Meta?.Category ?? "");
        WriteString(Writer, Meta?.ToolTip ?? "");
        WriteString(Writer, Meta?.Units ?? "");

        bool bHasMin = Meta?.HasClampMin ?? false;
        Writer.Write((byte)(bHasMin ? 1 : 0));
        if (bHasMin)
        {
            Writer.Write((double)Meta!.ClampMin);
        }

        bool bHasMax = Meta?.HasClampMax ?? false;
        Writer.Write((byte)(bHasMax ? 1 : 0));
        if (bHasMax)
        {
            Writer.Write((double)Meta!.ClampMax);
        }

        Writer.Write((byte)((Meta?.Has(ESpecifier.ColorPicker) ?? false) ? 1 : 0));
        Writer.Write((byte)(bHidden ? 1 : 0));

        // Appended, which the Meta record's length prefix makes safe against an older native reader.
        Writer.Write((uint)ExtraFlags | (uint)ParamFlags);
        Writer.Write((double)NetRate);

        Writer.Write(ExtraMeta?.Count ?? 0);
        if (ExtraMeta != null)
        {
            foreach (KeyValuePair<string, string> Pair in ExtraMeta)
            {
                WriteString(Writer, Pair.Key);
                WriteString(Writer, Pair.Value);
            }
        }
    }

    // A field type in lockstep with the native ReadType, naming any struct, enum or class it refers to.
    private static void WriteType(BinaryWriter Writer, ScriptType Type)
    {
        using var Frame = new Record(Writer, ERecord.Type);

        Writer.Write((byte)Type.Kind);
        Writer.Write((byte)(Type.IsInputAction ? 1 : 0));
        switch (Type.Kind)
        {
            // A field names the reflected type it holds, which is minted on its own and found by that name.
            case EPropertyType.Enum:
            {
                WriteString(Writer, Type.EnumName ?? "");
                Writer.Write((byte)Type.EnumUnderlying);
                break;
            }
            case EPropertyType.Struct:
            case EPropertyType.ComponentRef:
            {
                WriteString(Writer, Type.NativeName ?? "");
                break;
            }
            case EPropertyType.Object:
            case EPropertyType.SoftObject:
            {
                WriteString(Writer, Type.TargetClass ?? "");
                break;
            }
            case EPropertyType.Vector:
            case EPropertyType.Optional:
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
                break;
            }
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

    internal static void WriteString(BinaryWriter Writer, string Value)
    {
        byte[] Bytes = Encoding.UTF8.GetBytes(Value);
        Writer.Write(Bytes.Length);
        Writer.Write(Bytes);
    }
}
