#include <gtest/gtest.h>

#include "Containers/Vector.h"
#include "Scripting/ScriptSchemaCodec.h"
#include "Scripting/ScriptSchemaWire.h"

using namespace Lumina;
using namespace Lumina::Scripting;

namespace
{
    // Mirrors LuminaSharp.Serializer so a test can produce the wire the managed side would. Kept deliberately
    // separate from the reader: a bug that cancelled out between one shared implementation and itself would
    // pass, which is exactly the drift this format exists to catch.
    struct FTestSchemaWriter
    {
        TVector<uint8> Bytes;

        void Raw(const void* P, size_t N) { Bytes.insert(Bytes.end(), (const uint8*)P, (const uint8*)P + N); }
        void U8(uint8 V)   { Bytes.push_back(V); }
        void U16(uint16 V) { Raw(&V, 2); }
        void U32(uint32 V) { Raw(&V, 4); }
        void I32(int32 V)  { Raw(&V, 4); }
        void I64(int64 V)  { Raw(&V, 8); }
        void F64(double V) { Raw(&V, 8); }
        void Str(const char* S)
        {
            const int32 Len = (int32)strlen(S);
            I32(Len);
            Raw(S, (size_t)Len);
        }

        void Header(uint32 Magic = kScriptSchemaMagic, uint16 Version = kScriptSchemaVersion)
        {
            U32(Magic);
            U16(Version);
        }

        // Opens a record and returns where its length must be backfilled.
        size_t Open(EScriptSchemaRecord Tag)
        {
            U8((uint8)Tag);
            const size_t LengthPos = Bytes.size();
            U32(0);
            return LengthPos;
        }

        void Close(size_t LengthPos)
        {
            const uint32 Length = (uint32)(Bytes.size() - LengthPos - sizeof(uint32));
            memcpy(Bytes.data() + LengthPos, &Length, sizeof(uint32));
        }

        // bWriteFlags off reproduces a managed side older than the flags field, which must still parse.
        void Meta(const char* Category = "", const char* Tooltip = "", uint32 Flags = 0, bool bWriteFlags = false)
        {
            const size_t L = Open(EScriptSchemaRecord::Meta);
            Str(Category);
            Str(Tooltip);
            Str("");        // Units
            U8(0);          // no ClampMin
            U8(0);          // no ClampMax
            U8(0);          // not Color
            U8(0);          // not hidden
            if (bWriteFlags)
            {
                U32(Flags);
            }
            Close(L);
        }

        void ScalarType(EPropertyTypeFlags Kind)
        {
            const size_t L = Open(EScriptSchemaRecord::Type);
            U8((uint8)Kind);
            U8(0);          // not an entity
            U8(0);          // not an input action
            Close(L);
        }

        void NilValue() { U8((uint8)EScriptValueKind::Nil); }

        // Only a top-level field carries the hot-reload byte, so a nested one that wrote it would put every
        // reader a byte out from there on.
        void ScalarField(const char* Name, EPropertyTypeFlags Kind, const char* Tooltip = "",
                         uint32 Flags = 0, bool bWriteFlags = false, bool bTopLevel = true)
        {
            const size_t L = Open(EScriptSchemaRecord::Field);
            Str(Name);
            I32(0);                 // no aliases
            Meta("", Tooltip, Flags, bWriteFlags);
            if (bTopLevel)
            {
                U8(0);              // not SkipHotReload
            }
            ScalarType(Kind);
            NilValue();
            Close(L);
        }

        void Function(const char* Name, int32 ReturnIndex, const TVector<const char*>& ParamNames,
                      const TVector<EPropertyTypeFlags>& ParamKinds)
        {
            const size_t L = Open(EScriptSchemaRecord::Function);
            Str(Name);
            I32(ReturnIndex);
            I32((int32)ParamNames.size());
            for (size_t i = 0; i < ParamNames.size(); ++i)
            {
                ScalarField(ParamNames[i], ParamKinds[i], "", 0, false, /*bTopLevel*/ false);
            }
            Close(L);
        }
    };
}

// Functions are written after the fields, so a buffer that has none is exactly what it was before they existed.
TEST(ScriptSchemaCodec, ASchemaWithNoFunctionsStillParses)
{
    FTestSchemaWriter W;
    W.Header();
    W.I32(1);
    W.ScalarField("Speed", EPropertyTypeFlags::Float);

    FScriptExportSchema Schema;
    TVector<FScriptPropertyEntry> Defaults;
    ASSERT_TRUE(ParseSchemaBlob(W.Bytes, Schema, Defaults));

    EXPECT_EQ(Schema.Fields.size(), 1u);
    EXPECT_TRUE(Schema.Functions.empty());
}

TEST(ScriptSchemaCodec, ADeclaredFunctionRoundTripsWithItsParameters)
{
    FTestSchemaWriter W;
    W.Header();
    W.I32(1);
    W.ScalarField("Speed", EPropertyTypeFlags::Float);

    W.I32(1);
    W.Function("Add", 2, { "A", "B", "ReturnValue" },
        { EPropertyTypeFlags::Int32, EPropertyTypeFlags::Int32, EPropertyTypeFlags::Int32 });

    FScriptExportSchema Schema;
    TVector<FScriptPropertyEntry> Defaults;
    ASSERT_TRUE(ParseSchemaBlob(W.Bytes, Schema, Defaults));

    ASSERT_EQ(Schema.Functions.size(), 1u);
    const FScriptExportFunction& Function = Schema.Functions[0];

    EXPECT_EQ(Function.Name, FName("Add"));
    EXPECT_EQ(Function.ReturnIndex, 2);
    ASSERT_EQ(Function.Params.size(), 3u);
    EXPECT_EQ(Function.Params[0].Name, FName("A"));
    EXPECT_EQ(Function.Params[2].Name, FName("ReturnValue"));

    // The fields are untouched by what follows them, which is what makes appending safe.
    EXPECT_EQ(Schema.Fields.size(), 1u);
}

TEST(ScriptSchemaCodec, SeveralFunctionsKeepTheirOrder)
{
    FTestSchemaWriter W;
    W.Header();
    W.I32(1);
    W.ScalarField("Speed", EPropertyTypeFlags::Float);

    W.I32(2);
    W.Function("First", -1, {}, {});
    W.Function("Second", 0, { "Out" }, { EPropertyTypeFlags::Bool });

    FScriptExportSchema Schema;
    TVector<FScriptPropertyEntry> Defaults;
    ASSERT_TRUE(ParseSchemaBlob(W.Bytes, Schema, Defaults));

    ASSERT_EQ(Schema.Functions.size(), 2u);
    EXPECT_EQ(Schema.Functions[0].Name, FName("First"));
    EXPECT_EQ(Schema.Functions[0].ReturnIndex, -1);
    EXPECT_TRUE(Schema.Functions[0].Params.empty());
    EXPECT_EQ(Schema.Functions[1].Name, FName("Second"));
    EXPECT_EQ(Schema.Functions[1].ReturnIndex, 0);
}

TEST(ScriptSchemaCodec, AWellFormedBufferRoundTrips)
{
    FTestSchemaWriter W;
    W.Header();
    W.I32(2);
    W.ScalarField("Speed", EPropertyTypeFlags::Float, "how fast");
    W.ScalarField("Health", EPropertyTypeFlags::Int32);

    FScriptExportSchema Schema;
    TVector<FScriptPropertyEntry> Defaults;
    ASSERT_TRUE(ParseSchemaBlob(W.Bytes, Schema, Defaults));

    ASSERT_EQ(Schema.Fields.size(), 2u);
    EXPECT_EQ(Schema.Fields[0].Name, FName("Speed"));
    EXPECT_EQ(Schema.Fields[0].Type->Kind, EPropertyTypeFlags::Float);
    EXPECT_EQ(Schema.Fields[1].Name, FName("Health"));
    EXPECT_EQ(Schema.Fields[1].Type->Kind, EPropertyTypeFlags::Int32);

    const FString* Tooltip = Schema.Fields[0].Meta.Find(FName("ToolTip"));
    ASSERT_NE(Tooltip, nullptr);
    EXPECT_EQ(*Tooltip, FString("how fast"));

    EXPECT_EQ(Defaults.size(), 2u);
}

// The whole reason the format is framed: a writer that learns a new trailing field must not break a reader
// that has not. Without this the reader would consume the extra bytes as the next record and desync.
TEST(ScriptSchemaCodec, AnUnknownTrailingFieldIsSkippedRatherThanDesyncing)
{
    FTestSchemaWriter W;
    W.Header();
    W.I32(2);

    // A field whose METADATA record carries data this build does not know about.
    {
        const size_t L = W.Open(EScriptSchemaRecord::Field);
        W.Str("Speed");
        W.I32(0);
        {
            const size_t M = W.Open(EScriptSchemaRecord::Meta);
            W.Str("");
            W.Str("a tooltip");
            W.Str("");
            W.U8(0);
            W.U8(0);
            W.U8(0);
            W.U8(0);
            W.Str("something a later version added");   // unknown trailing payload
            W.I64(1234);
            W.Close(M);
        }
        W.U8(0);
        W.ScalarType(EPropertyTypeFlags::Float);
        W.NilValue();
        W.Close(L);
    }

    // A plain field after it, which only parses if the reader resynced correctly.
    W.ScalarField("Health", EPropertyTypeFlags::Int32);

    FScriptExportSchema Schema;
    TVector<FScriptPropertyEntry> Defaults;
    ASSERT_TRUE(ParseSchemaBlob(W.Bytes, Schema, Defaults));

    ASSERT_EQ(Schema.Fields.size(), 2u);
    EXPECT_EQ(Schema.Fields[0].Name, FName("Speed"));
    EXPECT_EQ(Schema.Fields[1].Name, FName("Health")) << "the reader did not resync past the unknown payload";
    EXPECT_EQ(Schema.Fields[1].Type->Kind, EPropertyTypeFlags::Int32);
}

TEST(ScriptSchemaCodec, ABufferThatIsNotASchemaIsRefused)
{
    FTestSchemaWriter W;
    W.Header(/*Magic*/ 0xDEADBEEFu);
    W.I32(0);

    FScriptExportSchema Schema;
    TVector<FScriptPropertyEntry> Defaults;
    EXPECT_FALSE(ParseSchemaBlob(W.Bytes, Schema, Defaults));
}

TEST(ScriptSchemaCodec, AVersionMismatchIsRefusedInsteadOfMisread)
{
    FTestSchemaWriter W;
    W.Header(kScriptSchemaMagic, (uint16)(kScriptSchemaVersion + 1));
    W.I32(1);
    W.ScalarField("Speed", EPropertyTypeFlags::Float);

    FScriptExportSchema Schema;
    TVector<FScriptPropertyEntry> Defaults;
    EXPECT_FALSE(ParseSchemaBlob(W.Bytes, Schema, Defaults));
}

TEST(ScriptSchemaCodec, ATruncatedBufferIsReportedRatherThanPartiallyAccepted)
{
    FTestSchemaWriter W;
    W.Header();
    W.I32(2);
    W.ScalarField("Speed", EPropertyTypeFlags::Float);
    W.ScalarField("Health", EPropertyTypeFlags::Int32);

    W.Bytes.resize(W.Bytes.size() - 12);

    FScriptExportSchema Schema;
    TVector<FScriptPropertyEntry> Defaults;
    EXPECT_FALSE(ParseSchemaBlob(W.Bytes, Schema, Defaults));
}

// A record written out of order is the shape a hand-mirrored writer drifts into, and the tag is what turns
// that into a named failure instead of four bytes read as a length.
TEST(ScriptSchemaCodec, AMisorderedRecordIsCaughtByItsTag)
{
    FTestSchemaWriter W;
    W.Header();
    W.I32(1);

    const size_t L = W.Open(EScriptSchemaRecord::Field);
    W.Str("Speed");
    W.I32(0);
    W.ScalarType(EPropertyTypeFlags::Float);   // a Type where the Meta record belongs
    W.U8(0);
    W.NilValue();
    W.Close(L);

    FScriptExportSchema Schema;
    TVector<FScriptPropertyEntry> Defaults;
    EXPECT_FALSE(ParseSchemaBlob(W.Bytes, Schema, Defaults));
}


// [Property(Flags = Replicated | ReadOnly)] has to survive the wire, because the alternative is a field the
// author marked replicated that silently is not.
TEST(ScriptSchemaCodec, DeclaredPropertyFlagsRoundTrip)
{
    constexpr uint32 Declared = (uint32)EPropertyFlags::Replicated | (uint32)EPropertyFlags::ReadOnly;

    FTestSchemaWriter W;
    W.Header();
    W.I32(2);
    W.ScalarField("Speed", EPropertyTypeFlags::Float, "", Declared, /*bWriteFlags*/ true);
    W.ScalarField("Health", EPropertyTypeFlags::Int32);

    FScriptExportSchema Schema;
    TVector<FScriptPropertyEntry> Defaults;
    ASSERT_TRUE(ParseSchemaBlob(W.Bytes, Schema, Defaults));

    ASSERT_EQ(Schema.Fields.size(), 2u);
    EXPECT_EQ(Schema.Fields[0].Flags, Declared);
    EXPECT_EQ(Schema.Fields[1].Flags, 0u) << "a writer that omits the field must read as no flags, not garbage";
}
