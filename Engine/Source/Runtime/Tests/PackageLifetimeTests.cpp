#include <gtest/gtest.h>

#include "Core/Object/Cast.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/MemoryFileSystem.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

namespace
{
    constexpr const char* kLifetimeAlias = "/PkgLife";

    void EnsureLifetimeMount()
    {
        static const bool bReady = []
        {
            RegisterPendingTypes();
            VFS::Mount<VFS::FMemoryFileSystem>(kLifetimeAlias);
            return true;
        }();
        (void)bReady;
    }

    void RetirePackage(CPackage* Package)
    {
        TVector<CObject*> Objects;
        GetObjectsWithPackage(Package, Objects);
        for (CObject* Object : Objects)
        {
            Object->ConditionalBeginDestroy();
        }
        Package->ExportTable.Reset();
        Package->ImportTable.Reset();
        Package->RemoveFromRoot();
    }

    // Saves Asset and Extra into a fresh file at Path, and leaves nothing of it in memory.
    void WriteTwoExportPackage(const char* Path, float AssetValue, float ExtraValue)
    {
        CPackage* Package = CPackage::CreatePackage(Path);
        ASSERT_NE(Package, nullptr);
        NewObject<CScriptableTest>(Package, FName("Asset"))->NativeValue = AssetValue;
        NewObject<CScriptableTest>(Package, FName("Extra"))->NativeValue = ExtraValue;
        ASSERT_TRUE(CPackage::SavePackage(Package, Path));
        RetirePackage(Package);
    }

    CScriptableTest* LoadNamed(CPackage* Package, const char* Name)
    {
        return Cast<CScriptableTest>(Package->LoadObjectByName(FName(Name)));
    }

    // Taking and dropping the only reference is what frees a resident object nothing else holds.
    void DropLastReference(CObject* Object)
    {
        TStrongObjectPtr<CObject> Held(Object);
    }
}

// An export whose holders all let go while its package had unsaved edits is saved with those edits, not reread from the old file.
TEST(PackageLifetime, AnExportReleasedWhileDirtyIsSavedWithItsEdits)
{
    EnsureLifetimeMount();
    constexpr const char* Path = "/PkgLife/Released.lasset";
    WriteTwoExportPackage(Path, 1.0f, 2.0f);

    CPackage* Package = CPackage::LoadPackage(Path);
    ASSERT_NE(Package, nullptr);
    CScriptableTest* Asset = LoadNamed(Package, "Asset");
    ASSERT_NE(Asset, nullptr);
    const FGuid AssetGuid = Asset->GetGUID();

    {
        TStrongObjectPtr<CScriptableTest> Edit(Asset);
        Edit->NativeValue = 7.0f;
        Package->MarkDirty();
    }

    CScriptableTest* Kept = Cast<CScriptableTest>(FindObjectImpl(AssetGuid));
    ASSERT_EQ(Kept, Asset) << "the dirty package let its edited export die";
    EXPECT_FLOAT_EQ(Kept->NativeValue, 7.0f);

    ASSERT_TRUE(CPackage::SavePackage(Package, Path));
    EXPECT_FALSE(Package->IsDirty());
    EXPECT_EQ(FindObjectImpl(AssetGuid), nullptr) << "a clean package should stop holding an export nothing else does";

    RetirePackage(Package);
    CPackage* Reloaded = CPackage::LoadPackage(Path);
    ASSERT_NE(Reloaded, nullptr);
    CScriptableTest* ReloadedAsset = LoadNamed(Reloaded, "Asset");
    ASSERT_NE(ReloadedAsset, nullptr);
    EXPECT_FLOAT_EQ(ReloadedAsset->NativeValue, 7.0f);
    RetirePackage(Reloaded);
}

// An object created since the last save is no export yet, so dropping it while dirty frees it rather than saving it.
TEST(PackageLifetime, AnObjectNeverSavedIsNotKeptForTheSave)
{
    EnsureLifetimeMount();
    constexpr const char* Path = "/PkgLife/Unsaved.lasset";
    WriteTwoExportPackage(Path, 1.0f, 2.0f);

    CPackage* Package = CPackage::LoadPackage(Path);
    ASSERT_NE(Package, nullptr);
    ASSERT_TRUE(Package->FullyLoad());

    CScriptableTest* Scratch = NewObject<CScriptableTest>(Package, FName("Scratch"));
    const FGuid ScratchGuid = Scratch->GetGUID();
    Package->MarkDirty();
    DropLastReference(Scratch);
    EXPECT_EQ(FindObjectImpl(ScratchGuid), nullptr);

    ASSERT_TRUE(CPackage::SavePackage(Package, Path));
    EXPECT_EQ(Package->ExportTable.Num(), 2);
    RetirePackage(Package);
}

// A cook writes a layout the package never holds, so an export reloaded afterwards still reads the editor bytes correctly.
TEST(PackageLifetime, CookingLeavesTheEditorLayoutInPlace)
{
    EnsureLifetimeMount();
    constexpr const char* Path = "/PkgLife/Cooked.lasset";
    WriteTwoExportPackage(Path, 3.0f, 4.0f);

    CPackage* Package = CPackage::LoadPackage(Path);
    ASSERT_NE(Package, nullptr);
    ASSERT_TRUE(Package->FullyLoad());
    const TVector<FObjectExport> EditorExports = Package->ExportTable;

    TVector<uint8> Cooked;
    ASSERT_TRUE(CPackage::SavePackageForCook(Package, Cooked, [](const CObject* Object) { return Object->GetName() == FName("Asset"); }));

    ASSERT_EQ(Package->ExportTable.Num(), EditorExports.Num());
    for (int32 i = 0; i < EditorExports.Num(); ++i)
    {
        EXPECT_EQ(Package->ExportTable[i].ObjectGUID, EditorExports[i].ObjectGUID);
        EXPECT_EQ(Package->ExportTable[i].Offset, EditorExports[i].Offset);
        const CObject* Object = Package->ExportTable[i].Object.Get();
        ASSERT_NE(Object, nullptr);
        EXPECT_EQ(Object->GetLoaderIndex(), FObjectPackageIndex::FromExport(i).GetRaw());
    }

    DropLastReference(LoadNamed(Package, "Asset"));
    DropLastReference(LoadNamed(Package, "Extra"));

    CScriptableTest* Asset = LoadNamed(Package, "Asset");
    CScriptableTest* Extra = LoadNamed(Package, "Extra");
    ASSERT_NE(Asset, nullptr);
    ASSERT_NE(Extra, nullptr);
    EXPECT_FLOAT_EQ(Asset->NativeValue, 3.0f);
    EXPECT_FLOAT_EQ(Extra->NativeValue, 4.0f);
    RetirePackage(Package);
}

// A save that never reached disk puts back the layout the resident bytes follow, or a later reload reads them at the new offsets.
TEST(PackageLifetime, AFailedSaveKeepsTheLayoutOnDisk)
{
    EnsureLifetimeMount();
    constexpr const char* Path = "/PkgLife/FailedSave.lasset";
    WriteTwoExportPackage(Path, 5.0f, 6.0f);

    CPackage* Package = CPackage::LoadPackage(Path);
    ASSERT_NE(Package, nullptr);
    ASSERT_TRUE(Package->FullyLoad());
    const TVector<FObjectExport> SavedExports = Package->ExportTable;

    CScriptableTest* Added = NewObject<CScriptableTest>(Package, FName("Added"));
    TStrongObjectPtr<CScriptableTest> HoldAdded(Added);
    Package->MarkDirty();

    EXPECT_FALSE(CPackage::SavePackage(Package, "/PkgLifeNotMounted/FailedSave.lasset"));

    ASSERT_EQ(Package->ExportTable.Num(), SavedExports.Num());
    for (int32 i = 0; i < SavedExports.Num(); ++i)
    {
        EXPECT_EQ(Package->ExportTable[i].ObjectGUID, SavedExports[i].ObjectGUID);
        EXPECT_EQ(Package->ExportTable[i].Offset, SavedExports[i].Offset);
    }
    EXPECT_EQ(Added->GetLoaderIndex(), 0);

    // Abandoning the edits makes the file the truth again, so a released export is reread from it.
    Package->ClearDirty();
    DropLastReference(LoadNamed(Package, "Extra"));
    CScriptableTest* Extra = LoadNamed(Package, "Extra");
    ASSERT_NE(Extra, nullptr);
    EXPECT_FLOAT_EQ(Extra->NativeValue, 6.0f);

    HoldAdded.Reset();
    RetirePackage(Package);
}

// A path whose load failed is loaded again once the file appears, and an asset created there takes the empty package over.
TEST(PackageLifetime, AFailedLoadDoesNotClaimThePath)
{
    EnsureLifetimeMount();
    constexpr const char* Source = "/PkgLife/LateSource.lasset";
    constexpr const char* Late   = "/PkgLife/Late.lasset";
    constexpr const char* Fresh  = "/PkgLife/Fresh.lasset";
    WriteTwoExportPackage(Source, 8.0f, 9.0f);

    EXPECT_EQ(CPackage::LoadPackage(Late), nullptr);
    TVector<uint8> Bytes;
    ASSERT_TRUE(VFS::ReadFile(Bytes, Source));
    ASSERT_TRUE(VFS::WriteFile(Late, TSpan<const uint8>(Bytes.GetData(), Bytes.Num())));

    CPackage* Loaded = CPackage::LoadPackage(Late);
    ASSERT_NE(Loaded, nullptr) << "a failed load kept refusing the path after its file appeared";
    CScriptableTest* Extra = LoadNamed(Loaded, "Extra");
    ASSERT_NE(Extra, nullptr);
    EXPECT_FLOAT_EQ(Extra->NativeValue, 9.0f);
    RetirePackage(Loaded);

    EXPECT_EQ(CPackage::LoadPackage(Fresh), nullptr);
    CPackage* Created = CPackage::CreatePackage(Fresh);
    ASSERT_NE(Created, nullptr) << "a failed load left a package that blocks creating an asset at its path";
    NewObject<CScriptableTest>(Created, FName("Fresh"));
    ASSERT_TRUE(CPackage::SavePackage(Created, Fresh));
    RetirePackage(Created);
    CPackage* Reloaded = CPackage::LoadPackage(Fresh);
    ASSERT_NE(Reloaded, nullptr);
    EXPECT_NE(LoadNamed(Reloaded, "Fresh"), nullptr);
    RetirePackage(Reloaded);
}

// The registry decodes the bytes it already read for the hash, which must come out the same as reading the file.
TEST(PackageLifetime, DecodingReadBytesMatchesReadingTheFile)
{
    EnsureLifetimeMount();
    constexpr const char* Path = "/PkgLife/Decode.lasset";
    WriteTwoExportPackage(Path, 1.0f, 2.0f);

    TVector<uint8> FromFile;
    FPackageHeader FileHeader{};
    ASSERT_TRUE(CPackage::ReadPackageContainer(Path, FromFile, FileHeader));

    TVector<uint8> Raw;
    ASSERT_TRUE(VFS::ReadFile(Raw, Path));
    TVector<uint8> FromBytes;
    FPackageHeader BytesHeader{};
    ASSERT_TRUE(CPackage::DecodePackageFile(Move(Raw), Path, FromBytes, BytesHeader));

    EXPECT_EQ(FromBytes, FromFile);
    EXPECT_EQ(BytesHeader.ExportCount, FileHeader.ExportCount);
}
