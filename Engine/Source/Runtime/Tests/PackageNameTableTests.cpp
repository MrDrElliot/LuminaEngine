#include <gtest/gtest.h>

#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Containers/Name.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/Properties/PropertyTag.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/Package/PackageNameTable.h"
#include "Core/Versioning/CoreVersion.h"

using namespace Lumina;

namespace
{
    constexpr int32 kNameTableVersion = (int32)ELuminaEngineVersion::PACKAGE_NAME_TABLE;

    // stands in for FPackageSaver and FPackageLoader, which need a CPackage behind them
    class FNameMapWriter : public FMemoryWriter
    {
    public:
        explicit FNameMapWriter(TVector<uint8>& InBytes) : FMemoryWriter(InBytes) {}

        using FArchive::operator<<;
        FArchive& operator<<(FName& Value) override
        {
            SerializePackageName(*this, Value, &Map, nullptr);
            return *this;
        }

        FPackageNameMap Map;
    };

    class FNameMapReader : public FMemoryReader
    {
    public:
        FNameMapReader(const TVector<uint8>& InBytes, const FPackageNameTable& InTable)
            : FMemoryReader(InBytes), Table(&InTable) {}

        using FArchive::operator<<;
        FArchive& operator<<(FName& Value) override
        {
            SerializePackageName(*this, Value, nullptr, Table);
            return *this;
        }

        const FPackageNameTable* Table;
    };
}

TEST(PackageNameTable, ANameSurvivesTheSlotRoundTrip)
{
    // NAME_None renders as text, so a table storing it verbatim reads back a real name of that spelling
    TVector<FName> Written = { "Alpha", "Beta", NAME_None, "Gamma", "Alpha", NAME_None };

    TVector<uint8>    Stream;
    TVector<uint8>    TableBytes;
    FPackageNameTable Table;

    {
        FNameMapWriter Writer(Stream);
        for (FName& Name : Written)
        {
            Writer << Name;
        }

        FMemoryWriter TableWriter(TableBytes);
        Writer.Map.Serialize(TableWriter);
    }

    {
        FMemoryReader TableReader(TableBytes);
        Table.Serialize(TableReader);
    }

    EXPECT_EQ(Table.Num(), 4u) << "a name used twice must take one slot";

    FNameMapReader Reader(Stream, Table);
    for (const FName& Expected : Written)
    {
        FName Read;
        Reader << Read;
        EXPECT_EQ(Read, Expected);
        EXPECT_EQ(Read.IsNone(), Expected.IsNone());
    }
}

// numbers travel beside the slot, so every uniquely-numbered export name shares one table entry
TEST(PackageNameTable, ANumberedNameSharesItsBaseSlot)
{
    TVector<FName> Written = { FName("Emitter", 0), FName("Emitter", 7), FName("Emitter"), FName("Emitter", 4711) };

    TVector<uint8>    Stream;
    TVector<uint8>    TableBytes;
    FPackageNameTable Table;

    {
        FNameMapWriter Writer(Stream);
        for (FName& Name : Written)
        {
            Writer << Name;
        }
        FMemoryWriter TableWriter(TableBytes);
        Writer.Map.Serialize(TableWriter);
    }

    {
        FMemoryReader TableReader(TableBytes);
        Table.Serialize(TableReader);
    }

    EXPECT_EQ(Table.Num(), 1u) << "the number is not part of the interned text";

    FNameMapReader Reader(Stream, Table);
    for (const FName& Expected : Written)
    {
        FName Read;
        Reader << Read;
        EXPECT_EQ(Read, Expected);
        EXPECT_EQ(Read.HasNumber(), Expected.HasNumber());
        EXPECT_EQ(Read.GetNumber(), Expected.GetNumber());
    }
}

// an absent suffix and an explicit "_0" both report GetNumber() == 0, so a flag carries the difference
TEST(PackageNameTable, AnExplicitZeroSuffixIsNotTheSameAsNoSuffix)
{
    const FName Plain("Slot");
    const FName Zero("Slot", 0);

    ASSERT_FALSE(Plain.HasNumber());
    ASSERT_TRUE(Zero.HasNumber());
    ASSERT_NE(Plain, Zero);
}

TEST(PackageNameTable, ASlotOutsideTheTableReadsAsNone)
{
    FPackageNameTable Empty;
    EXPECT_EQ(Empty.Resolve(0, false, 0), NAME_None);
    EXPECT_EQ(Empty.Resolve(9, false, 0), NAME_None);
}

TEST(PackageNameTable, ACorruptEntryCountIsRefused)
{
    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        uint32 Absurd = 0xFFFFFFFFu;
        Writer << Absurd;
    }

    FMemoryReader     Reader(Bytes);
    FPackageNameTable Table;
    Table.Serialize(Reader);

    EXPECT_TRUE(Reader.HasError());
    EXPECT_EQ(Table.Num(), 0u);
}

TEST(PropertyTag, ATagRoundTripsAtTheCurrentVersion)
{
    FPropertyTag Written;
    Written.Type = EPropertyTypeFlags::Float;
    Written.Name = "NoiseScale";
    Written.Size = 4;

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        Writer.SetFileVersion(kNameTableVersion);
        Writer << Written;
    }

    // 1 kind + the name + 4 size, where the name costs 8 bytes of text length before its 10 characters.
    EXPECT_LT(Bytes.size(), 24u) << "the tag is what the stream pays per property";

    FPropertyTag Read;
    FMemoryReader Reader(Bytes);
    Reader.SetFileVersion(kNameTableVersion);
    Reader << Read;

    EXPECT_EQ(Read.Type, Written.Type);
    EXPECT_EQ(Read.Name, Written.Name);
    EXPECT_EQ(Read.Size, Written.Size);
}

// files written before PACKAGE_NAME_TABLE spell the kind out and carry an offset nothing reads
TEST(PropertyTag, ALegacyTagStillDecodesItsKind)
{
    FPropertyTag Written;
    Written.Type = EPropertyTypeFlags::Struct;
    Written.Name = "ShapeSize";
    Written.Size = 12;

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        Writer.SetFileVersion(kNameTableVersion - 1);
        Writer << Written;
    }

    FPropertyTag Read;
    FMemoryReader Reader(Bytes);
    Reader.SetFileVersion(kNameTableVersion - 1);
    Reader << Read;

    EXPECT_EQ(Read.Type, EPropertyTypeFlags::Struct);
    EXPECT_EQ(Read.Name, Written.Name);
    EXPECT_EQ(Read.Size, Written.Size);
}

TEST(PropertyTag, EveryKindNameMapsBackToItsKind)
{
    for (uint16 Kind = 0; Kind < (uint16)EPropertyTypeFlags::Count; ++Kind)
    {
        const EPropertyTypeFlags Expected = (EPropertyTypeFlags)Kind;
        EXPECT_EQ(PropertyTypeFromName(FName(PropertyTypeToString(Expected))), Expected);
    }
}

// IsValueValidForType tested "IntProperty" where the real name is Int32Property, so int32 always failed
TEST(PropertyTag, EveryNumericKindAcceptsAValueInItsRange)
{
    const EPropertyTypeFlags Numeric[] = {
        EPropertyTypeFlags::Int8,  EPropertyTypeFlags::Int16,  EPropertyTypeFlags::Int32,  EPropertyTypeFlags::Int64,
        EPropertyTypeFlags::UInt8, EPropertyTypeFlags::UInt16, EPropertyTypeFlags::UInt32, EPropertyTypeFlags::UInt64,
        EPropertyTypeFlags::Float, EPropertyTypeFlags::Double,
    };

    for (const EPropertyTypeFlags Kind : Numeric)
    {
        EXPECT_TRUE(IsPropertyNumeric(Kind)) << PropertyTypeToString(Kind);
        EXPECT_TRUE(IsValueValidForType(7.0, Kind)) << PropertyTypeToString(Kind);
    }

    EXPECT_FALSE(IsValueValidForType(1e300, EPropertyTypeFlags::Int32));
    EXPECT_FALSE(IsValueValidForType(-1.0, EPropertyTypeFlags::UInt8));
    EXPECT_FALSE(IsPropertyNumeric(EPropertyTypeFlags::Struct));
}

// a real package through both encodings, which proves the version gate rather than the tag alone
namespace
{
    constexpr const char* kCompatAlias = "/PkgCompat";

    void EnsureCompatMount()
    {
        static const bool bReady = []
        {
            ProcessNewlyLoadedCObjects();
            VFS::Mount<VFS::FMemoryFileSystem>(kCompatAlias);
            return true;
        }();
        (void)bReady;
    }

    void RetireCompatPackage(CPackage* Package)
    {
        TVector<CObject*> Objects;
        GetObjectsWithPackage(Package, Objects);
        for (CObject* Object : Objects)
        {
            if (Object != Package)
            {
                Object->ConditionalBeginDestroy();
            }
        }
        Package->ExportTable.clear();
        Package->ImportTable.clear();
        Package->RemoveFromRoot();
        Package->ConditionalBeginDestroy();
    }

    void RoundTripAtVersion(const char* Tag, int32 ForcedVersion)
    {
        EnsureCompatMount();
        CParticleEmitter::StaticClass()->Link();

        char Path[128];
        std::snprintf(Path, sizeof(Path), "%s/%s.lasset", kCompatAlias, Tag);

        const FName ExportName("Emitter", 12);
        const FGuid Guid = FGuid::New();

        {
            CPackage* Package = CPackage::CreatePackage(Path);
            ASSERT_NE(Package, nullptr) << Tag;

            CParticleEmitter* Emitter = Cast<CParticleEmitter>(
                NewObject(CParticleEmitter::StaticClass(), Package, ExportName, Guid));
            ASSERT_NE(Emitter, nullptr) << Tag;

            Emitter->NoiseScale          = 7.25f;
            Emitter->ShapeSize           = FVector3(3.0f, 4.0f, 5.0f);
            Emitter->AuthoringStackName  = "A stack name longer than the small-string buffer";
            Emitter->bBillboardToCamera  = false;
            Emitter->MaxParticles        = 1234;

            SetForcedPackageSaveVersion(ForcedVersion);
            const bool bSaved = CPackage::SavePackage(Package, Path);
            SetForcedPackageSaveVersion(0);

            ASSERT_TRUE(bSaved) << Tag;
            RetireCompatPackage(Package);
        }

        CPackage* Loaded = CPackage::LoadPackage(Path);
        ASSERT_NE(Loaded, nullptr) << Tag;
        ASSERT_TRUE(Loaded->FullyLoad()) << Tag;

        CParticleEmitter* Read = Cast<CParticleEmitter>(FindObject<CObject>(Guid));
        ASSERT_NE(Read, nullptr) << Tag << ": the export did not come back";

        EXPECT_EQ(Read->GetName(), ExportName) << Tag << ": a numbered export name must survive";
        EXPECT_FLOAT_EQ(Read->NoiseScale, 7.25f) << Tag;
        EXPECT_FLOAT_EQ(Read->ShapeSize[0], 3.0f) << Tag;
        EXPECT_FLOAT_EQ(Read->ShapeSize[2], 5.0f) << Tag;
        EXPECT_EQ(Read->AuthoringStackName, "A stack name longer than the small-string buffer") << Tag;
        EXPECT_FALSE(Read->bBillboardToCamera) << Tag;
        EXPECT_EQ(Read->MaxParticles, 1234) << Tag;

        RetireCompatPackage(Loaded);
    }
}

TEST(PackageNameTable, APackageRoundTripsThroughTheNameTable)
{
    RoundTripAtVersion("Current", 0);
}

TEST(PackageNameTable, APackageWrittenBeforeTheNameTableStillLoads)
{
    RoundTripAtVersion("Legacy", kNameTableVersion - 1);
}

// the registry scan reads a container with no CPackage behind it, which is its own decode path
TEST(PackageNameTable, AContainerReaderDecodesTheExportTableWithoutAPackage)
{
    EnsureCompatMount();
    CParticleEmitter::StaticClass()->Link();

    const char* Path = "/PkgCompat/ContainerScan.lasset";
    const FName ExportName("ScannedEmitter", 3);
    FGuid Guid;

    {
        CPackage* Package = CPackage::CreatePackage(Path);
        ASSERT_NE(Package, nullptr);

        CObject* Emitter = NewObject(CParticleEmitter::StaticClass(), Package, ExportName, FGuid::New());
        ASSERT_NE(Emitter, nullptr);
        Guid = Emitter->GetGUID();

        ASSERT_TRUE(CPackage::SavePackage(Package, Path));
        RetireCompatPackage(Package);
    }

    TVector<uint8> Bytes;
    ASSERT_TRUE(CPackage::ReadPackageFile(Path, Bytes));

    FPackageHeader Header;
    FPackageContainerReader Reader(Bytes);
    Reader << Header;
    Reader.SetFileVersion(Header.Version);

    ASSERT_EQ(Header.Tag, (uint32)PACKAGE_FILE_TAG);
    ASSERT_GT(Header.NameTableOffset, 0) << "a current-version package must locate its name table";

    FPackageNameTable Names;
    ASSERT_TRUE(CPackage::ReadNameTable(Reader, Header, Names));
    EXPECT_GT(Names.Num(), 0u);
    Reader.SetNameTable(&Names);

    Reader.Seek(Header.ExportTableOffset);
    TVector<FObjectExport> Exports;
    Reader << Exports;

    ASSERT_FALSE(Exports.empty());

    const FObjectExport* Found = nullptr;
    for (const FObjectExport& Export : Exports)
    {
        if (Export.ObjectGUID == Guid)
        {
            Found = &Export;
            break;
        }
    }

    ASSERT_NE(Found, nullptr) << "the scan could not find the export it just wrote";
    EXPECT_EQ(Found->ObjectName, ExportName);
    EXPECT_EQ(Found->ClassName, CParticleEmitter::StaticClass()->GetName());
}

// reading the tables without standing the name table up first must not invent plausible names
TEST(PackageNameTable, AContainerReadWithoutItsNameTableDoesNotInventNames)
{
    EnsureCompatMount();
    CParticleEmitter::StaticClass()->Link();

    const char* Path = "/PkgCompat/NoTable.lasset";

    {
        CPackage* Package = CPackage::CreatePackage(Path);
        ASSERT_NE(Package, nullptr);
        ASSERT_NE(NewObject(CParticleEmitter::StaticClass(), Package, "Unscanned", FGuid::New()), nullptr);
        ASSERT_TRUE(CPackage::SavePackage(Package, Path));
        RetireCompatPackage(Package);
    }

    TVector<uint8> Bytes;
    ASSERT_TRUE(CPackage::ReadPackageFile(Path, Bytes));

    FPackageHeader Header;
    FPackageContainerReader Reader(Bytes);
    Reader << Header;
    Reader.SetFileVersion(Header.Version);

    Reader.Seek(Header.ExportTableOffset);
    TVector<FObjectExport> Exports;
    Reader << Exports;

    ASSERT_FALSE(Exports.empty());
    EXPECT_EQ(Exports[0].ObjectName, NAME_None) << "an unresolvable slot must read as none, not as a real name";
}
