#include <gtest/gtest.h>

#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Assets/AssetTypes/PhysicsMaterial/PhysicsMaterial.h"
#include "Containers/Algorithm.h"
#include "Containers/Name.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/Properties/PropertyHeader.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "Core/Serialization/Package/PackageNameTable.h"
#include "Core/Versioning/CoreVersion.h"
#include "Log/Log.h"
#include "LogCountTestHelpers.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "World/World.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    constexpr int32 kNameTableVersion = (int32)ELuminaEngineVersion::PACKAGE_NAME_TABLE;
}

TEST(PackageNameTable, ANameSurvivesTheSlotRoundTrip)
{
    // NAME_None renders as text, so a table storing it verbatim reads back a real name of that spelling
    TVector<FName> Written = { "Alpha", "Beta", NAME_None, "Gamma", "Alpha", NAME_None };

    TVector<uint8>    Stream;
    TVector<uint8>    TableBytes;
    FPackageNameTable Table;

    {
        FPackageContainerWriter Writer(Stream);
        for (FName& Name : Written)
        {
            Writer << Name;
        }

        FMemoryWriter TableWriter(TableBytes);
        Writer.GetNameMap().Serialize(TableWriter);
    }

    {
        FMemoryReader TableReader(TableBytes);
        Table.Serialize(TableReader);
    }

    EXPECT_EQ(Table.Num(), 4) << "a name used twice must take one slot";

    FPackageContainerReader Reader(Stream);
    Reader.SetNameTable(&Table);
    for (const FName& Expected : Written)
    {
        FName Read;
        Reader << Read;
        EXPECT_EQ(Read, Expected);
        EXPECT_EQ(Read.IsNone(), Expected.IsNone());
    }
}

TEST(PackageNameTable, ANameWithTwoNumericSuffixesKeepsBoth)
{
    TVector<FName> Written = { "SM_Tree_1_1", "SM_Tree_2_2" };

    TVector<uint8>    Stream;
    TVector<uint8>    TableBytes;
    FPackageNameTable Table;

    {
        FPackageContainerWriter Writer(Stream);
        for (FName& Name : Written)
        {
            Writer << Name;
        }

        FMemoryWriter TableWriter(TableBytes);
        Writer.GetNameMap().Serialize(TableWriter);
    }

    {
        FMemoryReader TableReader(TableBytes);
        Table.Serialize(TableReader);
    }

    FPackageContainerReader Reader(Stream);
    Reader.SetNameTable(&Table);
    for (const FName& Expected : Written)
    {
        FName Read;
        Reader << Read;
        EXPECT_EQ(Read.ToString(), Expected.ToString());
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
        FPackageContainerWriter Writer(Stream);
        for (FName& Name : Written)
        {
            Writer << Name;
        }
        FMemoryWriter TableWriter(TableBytes);
        Writer.GetNameMap().Serialize(TableWriter);
    }

    {
        FMemoryReader TableReader(TableBytes);
        Table.Serialize(TableReader);
    }

    EXPECT_EQ(Table.Num(), 1) << "the number is not part of the interned text";

    FPackageContainerReader Reader(Stream);
    Reader.SetNameTable(&Table);
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
    EXPECT_EQ(Table.Num(), 0);
}

TEST(PropertyHeader, RoundTripsAtTheCurrentVersion)
{
    FPropertyHeader Written;
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
    EXPECT_LT(Bytes.Num(), 24) << "the tag is what the stream pays per property";

    FPropertyHeader Read;
    FMemoryReader Reader(Bytes);
    Reader.SetFileVersion(kNameTableVersion);
    Reader << Read;

    EXPECT_EQ(Read.Type, Written.Type);
    EXPECT_EQ(Read.Name, Written.Name);
    EXPECT_EQ(Read.Size, Written.Size);
}

// files written before PACKAGE_NAME_TABLE spell the kind out and carry an offset nothing reads
TEST(PropertyHeader, ALegacyHeaderStillDecodesItsKind)
{
    FPropertyHeader Written;
    Written.Type = EPropertyTypeFlags::Struct;
    Written.Name = "ShapeSize";
    Written.Size = 12;

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        Writer.SetFileVersion(kNameTableVersion - 1);
        Writer << Written;
    }

    FPropertyHeader Read;
    FMemoryReader Reader(Bytes);
    Reader.SetFileVersion(kNameTableVersion - 1);
    Reader << Read;

    EXPECT_EQ(Read.Type, EPropertyTypeFlags::Struct);
    EXPECT_EQ(Read.Name, Written.Name);
    EXPECT_EQ(Read.Size, Written.Size);
}

TEST(PropertyHeader, EveryKindNameMapsBackToItsKind)
{
    for (uint16 Kind = 0; Kind < (uint16)EPropertyTypeFlags::Count; ++Kind)
    {
        const EPropertyTypeFlags Expected = (EPropertyTypeFlags)Kind;
        EXPECT_EQ(PropertyTypeFromName(FName(PropertyTypeToString(Expected))), Expected);
    }
}

// IsValueValidForType tested "IntProperty" where the real name is Int32Property, so int32 always failed
TEST(PropertyHeader, EveryNumericKindAcceptsAValueInItsRange)
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
            RegisterPendingTypes();
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
        Package->ExportTable.Reset();
        Package->ImportTable.Reset();
        // The root set holds the only strong reference, so unrooting is what destroys it.
        Package->RemoveFromRoot();
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

    ASSERT_EQ(Header.Tag, PACKAGE_FILE_TAG);
    ASSERT_GT(Header.NameTableOffset, 0) << "a current-version package must locate its name table";

    FPackageNameTable Names;
    ASSERT_TRUE(CPackage::ReadNameTable(Reader, Header, Names));
    EXPECT_GT(Names.Num(), 0);
    Reader.SetNameTable(&Names);

    Reader.Seek(Header.ExportTableOffset);
    TVector<FObjectExport> Exports;
    Reader << Exports;

    ASSERT_FALSE(Exports.IsEmpty());

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

namespace
{
    // Loads a saved emitter whose export is re-labeled as a class this process never registered, and counts the resolve errors.
    int32 CountErrorsLoadingUnregisteredClass(const char* Tag, const FName& ClassPackage)
    {
        EnsureCompatMount();
        CParticleEmitter::StaticClass()->Link();

        char Path[128];
        std::snprintf(Path, sizeof(Path), "%s/%s.lasset", kCompatAlias, Tag);
        const FGuid Guid = FGuid::New();
        const FName MissingClass("CUnregisteredNodeGraph");

        {
            CPackage* Package = CPackage::CreatePackage(Path);
            EXPECT_NE(NewObject(CParticleEmitter::StaticClass(), Package, "Graph", Guid), nullptr) << Tag;
            EXPECT_TRUE(CPackage::SavePackage(Package, Path)) << Tag;
            RetireCompatPackage(Package);
        }

        CPackage* Loaded = CPackage::LoadPackage(Path);
        EXPECT_NE(Loaded, nullptr) << Tag;
        FObjectExport* Export = Algo::FindIf(Loaded->ExportTable, [&](const FObjectExport& E) { return E.ObjectGUID == Guid; });
        EXPECT_NE(Export, nullptr) << Tag;
        EXPECT_EQ(Export->ClassPackage, CParticleEmitter::StaticClass()->GetPackage()->GetName()) << Tag << ": the save must record the class's type package";

        Export->ClassName    = MissingClass;
        Export->ClassPackage = ClassPackage;

        Logging::ClearLogQueue();
        EXPECT_EQ(Loaded->LoadObject(Guid), nullptr) << Tag;
        const int32 Errors = CountLoggedMessages(ELogLevel::Error, "CUnregisteredNodeGraph");

        RetireCompatPackage(Loaded);
        return Errors;
    }
}

TEST(PackageExportClass, EditorDataLoadsQuietlyWithoutTheEditorModule)
{
    ASSERT_EQ(FindObject<CPackage>(FName("/Types/Editor")), nullptr) << "the test binary must not register the editor's types";
    EXPECT_EQ(CountErrorsLoadingUnregisteredClass("EditorOnlyExport", "/Types/Editor"), 0);
}

TEST(PackageExportClass, AMissingRuntimeClassStillLogs)
{
    EXPECT_EQ(CountErrorsLoadingUnregisteredClass("MissingClassExport", CParticleEmitter::StaticClass()->GetPackage()->GetName()), 1);
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

    ASSERT_FALSE(Exports.IsEmpty());
    EXPECT_EQ(Exports[0].ObjectName, NAME_None) << "an unresolvable slot must read as none, not as a real name";
}

namespace
{
    // A map whose one entity has a box collider pointing at a second export of the same package.
    void SaveCompatWorld(const char* Path, const FGuid& WorldGuid, const FGuid& MaterialGuid, int32 ForcedVersion = 0)
    {
        EnsureCompatMount();
        SBoxColliderComponent::StaticStruct();
        RegisterPendingTypes();

        CPackage* Package = CPackage::CreatePackage(Path);
        ASSERT_NE(Package, nullptr) << Path;
        CWorld* World = Cast<CWorld>(NewObject(CWorld::StaticClass(), Package, "Map", WorldGuid));
        CPhysicsMaterial* Material = Cast<CPhysicsMaterial>(NewObject(CPhysicsMaterial::StaticClass(), Package, "Shape", MaterialGuid));
        ASSERT_NE(World, nullptr) << Path;
        ASSERT_NE(Material, nullptr) << Path;

        ECS::FRegistry& Registry = World->GetEntityRegistry();
        SBoxColliderComponent& Box = Registry.Emplace<SBoxColliderComponent>(Registry.Create());
        Box.HalfExtent = FVector3(3.0f);
        Box.PhysicsMaterial = Material;

        SetForcedPackageSaveVersion(ForcedVersion);
        const bool bSaved = CPackage::SavePackage(Package, Path);
        SetForcedPackageSaveVersion(0);

        EXPECT_TRUE(bSaved) << Path;

        // The collider's reference would otherwise free the material while the world is destroyed, ahead of its own retire.
        Registry.Clear();
        RetireCompatPackage(Package);
    }

    int32 NewerThanThisEngine()
    {
        return Global::GPackageFileLuminaVersion.FileVersion + 1;
    }

    // Writes some names as others, which is how an archive saved before a schema change reads today.
    class FRenamingWriter : public FInProcessObjectArchiver
    {
    public:

        FRenamingWriter(FArchive& Inner, const THashMap<FName, FName>& InRenames)
            : FInProcessObjectArchiver(Inner)
            , Renames(InRenames)
        {
        }

        using FInProcessObjectArchiver::operator<<;

        FArchive& operator<<(FName& Value) override
        {
            const auto Renamed = Renames.Find(Value);
            FName Written = Renamed != nullptr ? (*Renamed) : Value;
            return FInProcessObjectArchiver::operator<<(Written);
        }

    private:

        const THashMap<FName, FName>& Renames;
    };
}

// The collision shape asset type was removed, so maps that pointed at one must still open without it.
TEST(WorldLoad, AnExportOfAnUnresolvedClassLoadsAsNull)
{
    const char* Path = "/PkgCompat/UnresolvedClassWorld.lasset";
    const FGuid WorldGuid = FGuid::New();
    const FGuid MaterialGuid = FGuid::New();
    SaveCompatWorld(Path, WorldGuid, MaterialGuid);

    CPackage* Loaded = CPackage::LoadPackage(Path);
    ASSERT_NE(Loaded, nullptr);
    FObjectExport* Export = Algo::FindIf(Loaded->ExportTable, [&](const FObjectExport& E) { return E.ObjectGUID == MaterialGuid; });
    ASSERT_NE(Export, nullptr);
    Export->ClassName = FName("CCollisionShape");

    Logging::ClearLogQueue();
    CWorld* World = Cast<CWorld>(Loaded->LoadObject(WorldGuid));
    ASSERT_NE(World, nullptr);
    EXPECT_GE(CountLoggedMessages(ELogLevel::Error, "CCollisionShape"), 1);

    ECS::FRegistry& Registry = FWorldTestAccess::PendingRegistry(*World);
    int32 Boxes = 0;
    for (ECS::FEntity Entity : Registry.View<SBoxColliderComponent>())
    {
        const SBoxColliderComponent& Box = Registry.Get<SBoxColliderComponent>(Entity);
        EXPECT_FLOAT_EQ(Box.HalfExtent[0], 3.0f) << "the rest of the component must still load";
        EXPECT_EQ(Box.PhysicsMaterial.Get(), nullptr);
        ++Boxes;
    }
    EXPECT_EQ(Boxes, 1);

    RetireCompatPackage(Loaded);
}

TEST(WorldLoad, AWorldFromANewerEngineIsRefusedBeforeItsEntities)
{
    const char* Path = "/PkgCompat/NewerWorld.lasset";
    const FGuid WorldGuid = FGuid::New();
    SaveCompatWorld(Path, WorldGuid, FGuid::New(), NewerThanThisEngine());

    Logging::ClearLogQueue();
    EXPECT_EQ(CPackage::LoadPackage(Path), nullptr);
    EXPECT_EQ(FindObject<CObject>(WorldGuid), nullptr) << "an unreadable file must not hand out a world to deserialize";
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Error, "newer engine"), 1);

    if (CPackage* Refused = CPackage::FindPackageByPath(Path))
    {
        RetireCompatPackage(Refused);
    }
}

// A loaded package re-reads its file once the cached bytes are dropped, and that file may have been replaced since.
TEST(WorldLoad, AFileReplacedByANewerEngineIsRefusedOnReread)
{
    const char* Path = "/PkgCompat/RereadWorld.lasset";
    const char* NewerPath = "/PkgCompat/RereadWorldNewer.lasset";
    const FGuid WorldGuid = FGuid::New();

    SaveCompatWorld(NewerPath, FGuid::New(), FGuid::New(), NewerThanThisEngine());
    TVector<uint8> NewerFile;
    ASSERT_TRUE(VFS::ReadFile(NewerFile, NewerPath));
    SaveCompatWorld(Path, WorldGuid, FGuid::New());

    CPackage* Loaded = CPackage::LoadPackage(Path);
    ASSERT_NE(Loaded, nullptr);
    Loaded->LoaderBytes.reset();
    ASSERT_TRUE(VFS::WriteFile(Path, NewerFile));

    Logging::ClearLogQueue();
    CWorld* World = Cast<CWorld>(Loaded->LoadObject(WorldGuid));
    ASSERT_NE(World, nullptr);
    EXPECT_EQ(FWorldTestAccess::PendingRegistry(*World).NumEntities(), 0);
    EXPECT_EQ(CountLoggedMessages(ELogLevel::Error, "newer engine"), 1);

    RetireCompatPackage(Loaded);
}

// Properties and components that no longer exist are skipped, and the ones that remain still load.
TEST(WorldLoad, ComponentsWithRemovedPropertiesStillLoad)
{
    SSphereColliderComponent::StaticStruct();
    SBoxColliderComponent::StaticStruct();
    STransformComponent::StaticStruct();
    RegisterPendingTypes();

    CWorld* Source = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    ECS::FRegistry& SourceRegistry = Source->GetEntityRegistry();
    const ECS::FEntity Entity = SourceRegistry.Create();
    SSphereColliderComponent& Sphere = SourceRegistry.Emplace<SSphereColliderComponent>(Entity);
    Sphere.Radius = 4.0f;
    Sphere.TranslationOffset = FVector3(1.0f, 2.0f, 3.0f);
    SourceRegistry.Emplace<STransformComponent>(Entity);

    // A sphere read back as a box has a Radius the box lacks, and the transform reads as a type that is gone.
    const THashMap<FName, FName> Renames = {
        { StructIdentity(SSphereColliderComponent::StaticStruct()), StructIdentity(SBoxColliderComponent::StaticStruct()) },
        { StructIdentity(STransformComponent::StaticStruct()), FName("SRemovedComponent") },
    };

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        FRenamingWriter Renaming(Writer, Renames);
        Source->Serialize(Renaming);
    }

    Logging::ClearLogQueue();
    CWorld* Loaded = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    {
        FMemoryReader Reader(Bytes);
        FInProcessObjectArchiver Proxy(Reader);
        Loaded->Serialize(Proxy);
    }
    EXPECT_GE(CountLoggedMessages(ELogLevel::Warn, "Radius"), 1);

    ECS::FRegistry& Registry = FWorldTestAccess::PendingRegistry(*Loaded);
    ASSERT_EQ(Registry.NumEntities(), 1);
    int32 Boxes = 0;
    for (ECS::FEntity Read : Registry.View<SBoxColliderComponent>())
    {
        const SBoxColliderComponent& Box = Registry.Get<SBoxColliderComponent>(Read);
        EXPECT_FLOAT_EQ(Box.TranslationOffset[2], 3.0f);
        EXPECT_FLOAT_EQ(Box.HalfExtent[0], 0.5f) << "a property the file never had keeps its default";
        EXPECT_FALSE(Registry.HasAll<STransformComponent>(Read));
        ++Boxes;
    }
    EXPECT_EQ(Boxes, 1);
}
