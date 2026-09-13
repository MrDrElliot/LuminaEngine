#include "RuntimePCH.h"
#include "ScriptSchemaCodec.h"

#include <cstdio>

#include "Containers/Name.h"
#include "Core/Object/ObjectCore.h"
#include "Log/Log.h"
#include "Memory/SmartPtr.h"

namespace Lumina::Scripting
{
    namespace
    {
        // Little-endian cursor over the managed-written schema blob (see ScriptProperties.BuildSchemaBlob).
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

        // Every record states its tag and byte length first, so a reader that does not understand a trailing
        // field skips it and resyncs at the next boundary rather than misreading the rest of the buffer.
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

        // Append-cursor over a byte vector; mirror of the managed FBlobReader (little-endian).
        struct FBlobWriter
        {
            TVector<uint8>& B;
            void Raw(const void* P, size_t N) { B.insert(B.end(), (const uint8*)P, (const uint8*)P + N); }
            void U8(uint8 V) { B.push_back(V); }
            void I32(int32 V) { Raw(&V, 4); }
            void I64(int64 V) { Raw(&V, 8); }
            void F64(double V) { Raw(&V, 8); }
            void Str(FStringView S) { I32((int32)S.size()); Raw(S.data(), S.size()); }
        };

        // Folds a field's alias list into the field metadata as a ';'-joined "Aliases" value.
        void ReadAliasesInto(FBlobReader& R, FScriptExportMeta& Meta)
        {
            const int32 N = R.I32();
            FString Joined;
            for (int32 i = 0; i < N; ++i)
            {
                const FString Alias = R.Str();
                if (Alias.empty())
                {
                    continue;
                }
                if (!Joined.empty())
                {
                    Joined += ";";
                }
                Joined += Alias;
            }
            if (!Joined.empty())
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
            if (!Category.empty()) { Meta.Set("Category", Category); }
            if (!Tooltip.empty())  { Meta.Set("ToolTip", Tooltip); }
            if (!Units.empty())    { Meta.Set("Units", Units); }
            if (R.U8()) { Meta.Set("ClampMin", NumberToString(R.F64())); }
            if (R.U8()) { Meta.Set("ClampMax", NumberToString(R.F64())); }
            if (R.U8()) { Meta.Set("Color", FString()); }
            // [Serialize] rather than [Property]; the field gets storage and saving but no inspector row.
            if (R.U8()) { Meta.Set("ScriptHidden", FString()); }

            // Written by a newer managed side; an older buffer simply has no bytes left in this record.
            OutFlags = R.P < Scope.RecordEnd ? R.U32() : 0u;
        }

        void ReadValue(FBlobReader& R, FScriptPropertyValue& Out);

        FScriptExportField ReadField(FBlobReader& R, bool bTopLevel);

        TSharedPtr<FScriptExportType> ReadType(FBlobReader& R)
        {
            const FRecordScope Scope(R, EScriptSchemaRecord::Type);

            auto Type = MakeShared<FScriptExportType>();
            Type->Kind = static_cast<EPropertyTypeFlags>(R.U8());
            Type->bEntity = R.U8() != 0;
            Type->bInputAction = R.U8() != 0;
            switch (Type->Kind)
            {
                case EPropertyTypeFlags::Enum:
                {
                    Type->EnumName = FName(R.Str().c_str());
                    Type->EnumUnderlying = static_cast<EPropertyTypeFlags>(R.U8());
                    const int32 N = R.I32();
                    for (int32 i = 0; i < N; ++i)
                    {
                        FScriptEnumEntry E;
                        E.Name = FName(R.Str().c_str());
                        E.Value = R.I64();
                        Type->EnumEntries.push_back(E);
                    }
                    break;
                }
                case EPropertyTypeFlags::Struct:
                {
                    // The reader tells the two apart by whether a native name is present.
                    const FString NativeName = R.Str();
                    if (!NativeName.empty())
                    {
                        Type->NativeName = FName(NativeName.c_str());
                    }
                    Type->ManagedSize = (uint32)R.I32();
                    const int32 N = R.I32();
                    for (int32 i = 0; i < N; ++i)
                    {
                        Type->Fields.push_back(ReadField(R, /*bTopLevel*/ false));
                    }
                    break;
                }
                case EPropertyTypeFlags::SoftObject:
                {
                    Type->TargetClass = FName(R.Str().c_str());
                    break;
                }
                case EPropertyTypeFlags::Vector:
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
                    Type->BaseName = FName(R.Str().c_str());
                    const int32 NumCandidates = R.I32();
                    for (int32 c = 0; c < NumCandidates; ++c)
                    {
                        const FRecordScope CandidateScope(R, EScriptSchemaRecord::Candidate);

                        FScriptExportInstanceCandidate Candidate;
                        Candidate.TypeName = FName(R.Str().c_str());
                        const int32 NumFields = R.I32();
                        for (int32 i = 0; i < NumFields; ++i)
                        {
                            Candidate.Fields.push_back(ReadField(R, /*bTopLevel*/ false));
                        }
                        Type->Candidates.push_back(std::move(Candidate));
                    }
                    break;
                }
                default: break;
            }
            return Type;
        }

        // Recursive self-describing value reader; each value leads with its kind byte.
        void ReadValue(FBlobReader& R, FScriptPropertyValue& Out)
        {
            // (forward-declared above ReadType, which reads a default for every nested field)
            Out = FScriptPropertyValue{};
            Out.Kind = static_cast<EScriptValueKind>(R.U8());
            switch (Out.Kind)
            {
                case EScriptValueKind::Bool:   Out.AsBool = R.U8() != 0; break;
                case EScriptValueKind::Int:    Out.AsInt = R.I64(); break;
                case EScriptValueKind::Double: Out.AsDouble = R.F64(); break;
                case EScriptValueKind::String: Out.AsString = R.Str(); break;
                case EScriptValueKind::Array:
                {
                    const int32 N = R.I32();
                    Out.Items.reserve(N > 0 ? N : 0);
                    for (int32 i = 0; i < N; ++i)
                    {
                        FScriptPropertyValue E;
                        ReadValue(R, E);
                        Out.Items.push_back(E);
                    }
                    break;
                }
                case EScriptValueKind::Map:
                {
                    // count pairs, each [key, value]; stored interleaved in Items (Items[2i]=key, [2i+1]=value).
                    const int32 N = R.I32();
                    Out.Items.reserve(N > 0 ? N * 2 : 0);
                    for (int32 i = 0; i < N; ++i)
                    {
                        FScriptPropertyValue K;
                        ReadValue(R, K);
                        Out.Items.push_back(std::move(K));
                        FScriptPropertyValue Val;
                        ReadValue(R, Val);
                        Out.Items.push_back(std::move(Val));
                    }
                    break;
                }
                case EScriptValueKind::Nested:
                {
                    const int32 N = R.I32();
                    Out.StructFields.reserve(N > 0 ? N : 0);
                    for (int32 i = 0; i < N; ++i)
                    {
                        FScriptPropertyEntry E;
                        E.Name = FName(R.Str().c_str());
                        ReadValue(R, E.Value);
                        Out.StructFields.push_back(E);
                    }
                    break;
                }
                case EScriptValueKind::Instance:
                {
                    // Chosen type name, then (when non-empty) a field count and each field's name and value.
                    Out.AsString = R.Str();
                    if (!Out.AsString.empty())
                    {
                        const int32 N = R.I32();
                        Out.StructFields.reserve(N > 0 ? N : 0);
                        for (int32 i = 0; i < N; ++i)
                        {
                            FScriptPropertyEntry E;
                            E.Name = FName(R.Str().c_str());
                            ReadValue(R, E.Value);
                            Out.StructFields.push_back(E);
                        }
                    }
                    break;
                }
                default: break;
            }
        }

        // Mirror of ReadValue.
        void WriteValue(FBlobWriter& W, const FScriptPropertyValue& V)
        {
            W.U8((uint8)V.Kind);
            switch (V.Kind)
            {
                case EScriptValueKind::Bool:   W.U8(V.AsBool ? 1 : 0); break;
                case EScriptValueKind::Int:    W.I64(V.AsInt); break;
                case EScriptValueKind::Double: W.F64(V.AsDouble); break;
                case EScriptValueKind::String: W.Str(FStringView(V.AsString.c_str(), V.AsString.size())); break;
                case EScriptValueKind::Array:
                {
                    W.I32((int32)V.Items.size());
                    for (const FScriptPropertyValue& E : V.Items)
                    {
                        WriteValue(W, E);
                    }
                    break;
                }
                case EScriptValueKind::Map:
                {
                    // Items are the interleaved [key, value] pairs; the wire leads with the PAIR count.
                    W.I32((int32)(V.Items.size() / 2));
                    for (const FScriptPropertyValue& E : V.Items)
                    {
                        WriteValue(W, E);
                    }
                    break;
                }
                case EScriptValueKind::Nested:
                {
                    W.I32((int32)V.StructFields.size());
                    for (const FScriptPropertyEntry& E : V.StructFields)
                    {
                        W.Str(FStringView(E.Name.c_str()));
                        WriteValue(W, E.Value);
                    }
                    break;
                }
                case EScriptValueKind::Instance:
                {
                    W.Str(FStringView(V.AsString.c_str(), V.AsString.size()));
                    if (!V.AsString.empty())
                    {
                        W.I32((int32)V.StructFields.size());
                        for (const FScriptPropertyEntry& E : V.StructFields)
                        {
                            W.Str(FStringView(E.Name.c_str()));
                            WriteValue(W, E.Value);
                        }
                    }
                    break;
                }
                default: break;
            }
        }

        // The wire format is read in exactly one place, whichever managed export produced it.
        FScriptExportField ReadField(FBlobReader& R, bool bTopLevel)
        {
            const FRecordScope Scope(R, EScriptSchemaRecord::Field);

            FScriptExportField Field;
            Field.Name = FName(R.Str().c_str());
            ReadAliasesInto(R, Field.Meta);
            ReadMetaInto(R, Field.Meta, Field.Flags);

            // Only a top-level field has a hot-reload identity, so only it carries the byte.
            if (bTopLevel && R.U8())
            {
                Field.Meta.Set("SkipHotReload", FString());
            }

            Field.Type = ReadType(R);
            ReadValue(R, Field.Default);
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
                Out.Params.push_back(ReadField(R, /*bTopLevel*/ false));
            }

            return !R.bError && !Out.Name.IsNone();
        }

        bool ParseSchemaBlobInternal(const TVector<uint8>& Blob, FScriptExportSchema& OutSchema,
            TVector<FScriptPropertyEntry>& OutDefaults)
        {
            if (Blob.empty())
            {
                return false;
            }

            FBlobReader R{ Blob.data(), Blob.data() + Blob.size() };

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
                FScriptExportField Field = ReadField(R, /*bTopLevel*/ true);

                FScriptPropertyEntry Entry;
                Entry.Name = Field.Name;
                Entry.Value = Field.Default;

                OutSchema.Fields.push_back(std::move(Field));
                OutDefaults.push_back(std::move(Entry));
            }

            // Written after the fields, so a reader that stopped at their count simply never sees them and a
            // buffer from before functions existed leaves the list empty rather than failing.
            if (!R.bError && R.Remaining() > 0)
            {
                const int32 FunctionCount = R.I32();
                for (int32 i = 0; i < FunctionCount && !R.bError; ++i)
                {
                    FScriptExportFunction Function;
                    if (ReadFunction(R, Function))
                    {
                        OutSchema.Functions.push_back(std::move(Function));
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

    bool ParseSchemaBlob(const TVector<uint8>& Blob, FScriptExportSchema& OutSchema,
        TVector<FScriptPropertyEntry>& OutDefaults)
    {
        return ParseSchemaBlobInternal(Blob, OutSchema, OutDefaults);
    }

    void ParseButtonsBlob(const TVector<uint8>& Blob, TVector<FScriptButton>& OutButtons)
    {
        OutButtons.clear();
        if (Blob.empty())
        {
            return;
        }

        FBlobReader R{ Blob.data(), Blob.data() + Blob.size() };
        const int32 Count = R.I32();
        OutButtons.reserve(Count > 0 ? Count : 0);
        for (int32 i = 0; i < Count; ++i)
        {
            FScriptButton Button;
            Button.Method = R.Str();
            Button.Label = R.Str();
            Button.Tooltip = R.Str();
            OutButtons.push_back(std::move(Button));
        }
    }

    void WriteScriptValue(TVector<uint8>& OutBytes, const FScriptPropertyValue& Value)
    {
        FBlobWriter W{ OutBytes };
        WriteValue(W, Value);
    }
}
