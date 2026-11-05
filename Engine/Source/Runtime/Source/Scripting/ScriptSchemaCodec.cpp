#include "RuntimePCH.h"
#include "ScriptSchemaCodec.h"

#include <cstdio>

#include "Containers/Name.h"
#include "Core/Object/ObjectCore.h"
#include "ManagedTypeRegistry.h"
#include "Log/Log.h"
#include "Memory/SmartPtr.h"

namespace Lumina::Scripting
{
    namespace
    {
        // Little-endian cursor over the schema blob the managed Serializer writes.
        struct FBlobReader
        {
            const uint8* P;
            const uint8* End;
            bool         bError = false;

            bool Take(void* Dst, size_t N) { if (P + N > End) { bError = true; return false; } memcpy(Dst, P, N); P += N; return true; }
            int32   I32() { int32 V = 0; Take(&V, 4); return V; }
            int64   I64() { int64 V = 0; Take(&V, 8); return V; }
            double  F64() { double V = 0; Take(&V, 8); return V; }
            uint8   U8()  { uint8 V = 0; Take(&V, 1); return V; }
            uint16  U16() { uint16 V = 0; Take(&V, 2); return V; }
            uint32  U32() { uint32 V = 0; Take(&V, 4); return V; }

            void SeekTo(const uint8* Position) { P = Position < End ? Position : End; }

            size_t Remaining() const { return P < End ? (size_t)(End - P) : 0; }

            // Returning without advancing would desync the cursor and silently misread the tail.
            FString Str()
            {
                int32 N = I32();
                if (N == 0) { return FString(); }
                if (N < 0 || P + N > End) { P = End; return FString(); }
                FString S(reinterpret_cast<const char*>(P), static_cast<size_t>(N));
                P += N;
                return S;
            }
        };

        // Each record states its tag and byte length first, so a reader skips a field it does not know and resyncs.
        struct FRecordScope
        {
            FBlobReader&        Reader;
            const uint8*        RecordEnd;

            FRecordScope(FBlobReader& InReader, EScriptSchemaRecord Expected)
                : Reader(InReader)
            {
                const uint8 Tag = InReader.U8();
                const uint32 Length = InReader.U32();

                RecordEnd = InReader.P + Length;
                if (RecordEnd > InReader.End)
                {
                    RecordEnd = InReader.End;
                    InReader.bError = true;
                }

                if (Tag != (uint8)Expected)
                {
                    LOG_ERROR("Script schema desync: expected record {} but found {}.", (int)Expected, (int)Tag);
                    InReader.bError = true;
                }
            }

            LE_NO_COPYMOVE(FRecordScope);

            ~FRecordScope() { Reader.SeekTo(RecordEnd); }
        };

        FString NumberToString(double V) { char Buf[32]; snprintf(Buf, sizeof(Buf), "%g", V); return FString(Buf); }

        // Folds a field's alias list into the field metadata as a ';'-joined "Aliases" value.
        void ReadAliasesInto(FBlobReader& R, FScriptExportMeta& Meta)
        {
            const int32 N = R.I32();
            FString Joined;
            for (int32 i = 0; i < N; ++i)
            {
                const FString Alias = R.Str();
                if (Alias.IsEmpty())
                {
                    continue;
                }
                if (!Joined.IsEmpty())
                {
                    Joined += ";";
                }
                Joined += Alias;
            }
            if (!Joined.IsEmpty())
            {
                Meta.Set("Aliases", Joined);
            }
        }

        // Shared by the schema and every nested field, so the two read the identical bytes.
        void ReadMetaInto(FBlobReader& R, FScriptExportMeta& Meta, uint32& OutFlags)
        {
            const FRecordScope Scope(R, EScriptSchemaRecord::Meta);

            const FString Category = R.Str();
            const FString Tooltip  = R.Str();
            const FString Units    = R.Str();
            if (!Category.IsEmpty()) { Meta.Set("Category", Category); }
            if (!Tooltip.IsEmpty())  { Meta.Set("ToolTip", Tooltip); }
            if (!Units.IsEmpty())    { Meta.Set("Units", Units); }
            if (R.U8()) { Meta.Set("ClampMin", NumberToString(R.F64())); }
            if (R.U8()) { Meta.Set("ClampMax", NumberToString(R.F64())); }
            if (R.U8()) { Meta.Set("Color", FString()); }
            // A [Property(Hidden)], which is stored and saved but gets no inspector row.
            if (R.U8()) { Meta.Set("ScriptHidden", FString()); }

            // Written by a newer managed side; an older buffer simply has no bytes left in this record.
            OutFlags = R.P < Scope.RecordEnd ? R.U32() : 0u;

            const double NetRate = R.P < Scope.RecordEnd ? R.F64() : 0.0;
            if (NetRate > 0.0)
            {
                Meta.Set("Rate", NumberToString(NetRate));
            }

            // The rest of a [Property(Sync)] declaration, under the same keys PROPERTY(Sync = ...) writes for C++.
            const int32 Extra = R.P < Scope.RecordEnd ? R.I32() : 0;
            for (int32 i = 0; i < Extra && !R.bError && R.P < Scope.RecordEnd; ++i)
            {
                const FString Key = R.Str();
                const FString Value = R.Str();
                Meta.Set(FName(*Key), Value);
            }
        }

        FScriptExportField ReadField(FBlobReader& R, bool bTopLevel);

        TSharedPtr<FScriptExportType> ReadType(FBlobReader& R)
        {
            const FRecordScope Scope(R, EScriptSchemaRecord::Type);

            auto Type = MakeShared<FScriptExportType>();
            Type->Kind = static_cast<EPropertyTypeFlags>(R.U8());
            Type->bInputAction = R.U8() != 0;
            switch (Type->Kind)
            {
                case EPropertyTypeFlags::Enum:
                {
                    Type->EnumName = FName(*R.Str());
                    Type->EnumUnderlying = static_cast<EPropertyTypeFlags>(R.U8());
                    break;
                }
                case EPropertyTypeFlags::Struct:
                case EPropertyTypeFlags::ComponentRef:
                {
                    Type->NativeName = FName(*R.Str());
                    break;
                }
                case EPropertyTypeFlags::Object:
                case EPropertyTypeFlags::SoftObject:
                {
                    Type->TargetClass = FName(*R.Str());
                    break;
                }
                case EPropertyTypeFlags::Vector:
                case EPropertyTypeFlags::Optional:
                {
                    Type->ElementType = ReadType(R);
                    break;
                }
                case EPropertyTypeFlags::Map:
                {
                    Type->KeyType   = ReadType(R);
                    Type->ValueType = ReadType(R);
                    break;
                }
                case EPropertyTypeFlags::InstancedStruct:
                {
                    Type->BaseName = FName(*R.Str());
                    break;
                }
                default: break;
            }
            return Type;
        }

        // The wire format is read in exactly one place, whichever managed export produced it.
        FScriptExportField ReadField(FBlobReader& R, bool bTopLevel)
        {
            const FRecordScope Scope(R, EScriptSchemaRecord::Field);

            FScriptExportField Field;
            Field.Name = FName(*R.Str());
            ReadAliasesInto(R, Field.Meta);
            ReadMetaInto(R, Field.Meta, Field.Flags);

            // Only a top-level field has a hot-reload identity, so only it carries the byte.
            if (bTopLevel && R.U8())
            {
                Field.Meta.Set("SkipHotReload", FString());
            }

            Field.Type = ReadType(R);
            return Field;
        }

        // Its parameters are ordinary fields, so a call frame is described by exactly what describes a member.
        bool ReadFunction(FBlobReader& R, FScriptExportFunction& Out)
        {
            FRecordScope Scope(R, EScriptSchemaRecord::Function);
            if (R.bError)
            {
                return false;
            }

            Out.Name = FName(R.Str());
            Out.ReturnIndex = R.I32();

            const int32 Count = R.I32();
            for (int32 i = 0; i < Count && !R.bError; ++i)
            {
                Out.Params.Add(ReadField(R, /*bTopLevel*/ false));
            }
            Out.Flags = R.P < Scope.RecordEnd ? R.U32() : 0u;

            const int32 MetaCount = R.P < Scope.RecordEnd ? R.I32() : 0;
            for (int32 i = 0; i < MetaCount && !R.bError; ++i)
            {
                const FString Key = R.Str();
                Out.Meta.Set(FName(*Key), R.Str());
            }

            return !R.bError && !Out.Name.IsNone();
        }

        bool ParseSchemaBlobInternal(const TVector<uint8>& Blob, FScriptExportSchema& OutSchema)
        {
            if (Blob.IsEmpty())
            {
                return false;
            }

            FBlobReader R{ Blob.GetData(), Blob.GetData() + Blob.Num() };

            // Checked before a byte of payload is read, so a stale or foreign buffer is refused by name.
            const uint32 Magic = R.U32();
            const uint16 Version = R.U16();
            if (Magic != kScriptSchemaMagic)
            {
                LOG_ERROR("Script schema buffer is not a schema (magic {:#x}).", Magic);
                return false;
            }
            if (Version != kScriptSchemaVersion)
            {
                LOG_ERROR("Script schema version {} does not match the engine's {}; rebuild LuminaSharp.",
                    Version, kScriptSchemaVersion);
                return false;
            }

            const int32 Count = R.I32();
            for (int32 i = 0; i < Count; ++i)
            {
                OutSchema.Fields.Add(ReadField(R, /*bTopLevel*/ true));
            }

            // Written after the fields, so an older buffer without them leaves the list empty.
            if (!R.bError && R.Remaining() > 0)
            {
                const int32 FunctionCount = R.I32();
                for (int32 i = 0; i < FunctionCount && !R.bError; ++i)
                {
                    FScriptExportFunction Function;
                    if (ReadFunction(R, Function))
                    {
                        OutSchema.Functions.Add(std::move(Function));
                    }
                }
            }

            if (R.bError)
            {
                LOG_ERROR("Script schema buffer was truncated or desynced; the type is dropped.");
                return false;
            }
            return true;
        }
    }

    bool ParseSchemaBlob(const TVector<uint8>& Blob, FScriptExportSchema& OutSchema)
    {
        return ParseSchemaBlobInternal(Blob, OutSchema);
    }

    bool ParseTypeManifest(const TVector<uint8>& Blob, TVector<FManagedTypeDefinition>& OutTypes,
        TVector<FManagedTypeAlias>& OutAliases)
    {
        OutTypes.Reset();
        OutAliases.Reset();

        FBlobReader R{ Blob.GetData(), Blob.GetData() + Blob.Num() };
        if (R.U32() != kTypeManifestMagic || R.U16() != kTypeManifestVersion)
        {
            LOG_ERROR("Script type manifest has a bad header; the managed and native halves are out of date with each other.");
            return false;
        }

        const int32 TypeCount = R.I32();
        for (int32 Index = 0; Index < TypeCount && !R.bError; ++Index)
        {
            FManagedTypeDefinition& Type = OutTypes.EmplaceGetRef();
            Type.Kind = static_cast<EManagedTypeKind>(R.U8());
            Type.TypeName = FName(*R.Str());
            Type.NativeBaseName = R.Str();

            constexpr uint8 AbstractFlag = 1;
            Type.bAbstract = (R.U8() & AbstractFlag) != 0;

            const int32 OverrideCount = R.I32();
            for (int32 Override = 0; Override < OverrideCount && !R.bError; ++Override)
            {
                Type.OverriddenEvents.Add(R.Str());
            }

            const int32 MetaCount = R.I32();
            for (int32 Meta = 0; Meta < MetaCount && !R.bError; ++Meta)
            {
                FString Key = R.Str();
                FString Value = R.Str();
                Type.Meta.Emplace(std::move(Key), std::move(Value));
            }

            if (Type.Kind == EManagedTypeKind::Enum)
            {
                Type.EnumUnderlying = static_cast<EPropertyTypeFlags>(R.U8());
                const int32 EntryCount = R.I32();
                for (int32 Entry = 0; Entry < EntryCount && !R.bError; ++Entry)
                {
                    FScriptEnumEntry& Added = Type.EnumEntries.EmplaceGetRef();
                    Added.Name = FName(*R.Str());
                    Added.Value = R.I64();
                }
                continue;
            }

            const int32 SchemaLength = R.I32();
            if (SchemaLength < 0 || R.Remaining() < (size_t)SchemaLength)
            {
                R.bError = true;
                break;
            }
            const TVector<uint8> Schema(R.P, R.P + SchemaLength);
            R.SeekTo(R.P + SchemaLength);
            Type.bHasSchema = SchemaLength > 0 && ParseSchemaBlob(Schema, Type.Schema);
        }

        const int32 AliasCount = R.bError ? 0 : R.I32();
        for (int32 Index = 0; Index < AliasCount && !R.bError; ++Index)
        {
            FManagedTypeAlias& Alias = OutAliases.EmplaceGetRef();
            Alias.OldName = R.Str();
            Alias.NewName = R.Str();
        }

        if (R.bError)
        {
            LOG_ERROR("Script type manifest is truncated; no script types were published this load.");
            OutTypes.Reset();
            OutAliases.Reset();
            return false;
        }
        return true;
    }
}
