#include <gtest/gtest.h>
#include "Assets/AssetManager/AssetManager.h"
#include "Assets/AssetTypes/DataAsset/DataAsset.h"
#include "Containers/Algorithm.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/Package/PackageSaver.h"
#include "TaskSystem/CoroutineAssets.h"
#include "TaskSystem/ThreadedCallback.h"
#include "World/World.h"

using namespace Lumina;

namespace
{
    const FObjectImport* FindImport(const TVector<FObjectImport>& Imports, const FGuid& Guid)
    {
        const auto It = Algo::FindIf(Imports, [&](const FObjectImport& Import) { return Import.ObjectGUID == Guid; });
        return It != Imports.end() ? &*It : nullptr;
    }
}

TEST(AssetBundles, TheSaverRecordsEveryBundleThatNamesASoftReference)
{
    TVector<uint8> Bytes;
    FPackageSaver Saver(Bytes, nullptr);
    const FGuid Icon = FGuid::New();
    const FGuid Mesh = FGuid::New();
    const FGuid Plain = FGuid::New();

    const FStringView Outer = Saver.SetSoftReferenceBundles(" Icon , UI");
    Saver.RegisterSoftAssetReference(Icon);
    const FStringView Middle = Saver.SetSoftReferenceBundles("Visual");
    Saver.RegisterSoftAssetReference(Mesh);
    Saver.RegisterSoftAssetReference(Icon);
    Saver.SetSoftReferenceBundles(Middle);
    Saver.SetSoftReferenceBundles(Outer);
    Saver.RegisterSoftAssetReference(Plain);

    TVector<FObjectImport> Imports;
    Saver.PopulateImportTable(Imports);

    const FObjectImport* IconImport = FindImport(Imports, Icon);
    const FObjectImport* MeshImport = FindImport(Imports, Mesh);
    const FObjectImport* PlainImport = FindImport(Imports, Plain);
    ASSERT_NE(IconImport, nullptr);
    ASSERT_NE(MeshImport, nullptr);
    ASSERT_NE(PlainImport, nullptr);

    EXPECT_EQ(IconImport->Bundles, (TVector<FName>{ FName("Icon"), FName("UI"), FName("Visual") })) << "names are trimmed and a repeat is kept once";
    EXPECT_EQ(MeshImport->Bundles, (TVector<FName>{ FName("Visual") }));
    EXPECT_TRUE(PlainImport->Bundles.IsEmpty()) << "restoring the outer scope stops tagging";
}

TEST(AssetBundles, AnImportKeepsItsBundlesThroughASaveAndLoad)
{
    FObjectImport Written(FGuid::New(), EDependencyType::Soft);
    Written.Bundles = { FName("Icon"), FName("Gameplay") };

    TVector<uint8> Bytes;
    FMemoryWriter Writer(Bytes);
    Writer << Written;

    FObjectImport Read;
    FMemoryReader Reader(Bytes);
    Reader << Read;

    EXPECT_EQ(Read.ObjectGUID, Written.ObjectGUID);
    EXPECT_EQ(Read.Type, EDependencyType::Soft);
    EXPECT_EQ(Read.Bundles, Written.Bundles);
}

TEST(AssetBundles, AnImportFromBeforeBundlesReadsWithNone)
{
    FObjectImport Written(FGuid::New(), EDependencyType::Soft);
    Written.Bundles = { FName("Icon") };

    TVector<uint8> Bytes;
    FMemoryWriter Writer(Bytes);
    Writer.SetFileVersion((int32)ELuminaEngineVersion::ASSET_BUNDLES - 1);
    Writer << Written;

    FObjectImport Read;
    FMemoryReader Reader(Bytes);
    Reader.SetFileVersion((int32)ELuminaEngineVersion::ASSET_BUNDLES - 1);
    Reader << Read;

    EXPECT_EQ(Read.ObjectGUID, Written.ObjectGUID);
    EXPECT_TRUE(Read.Bundles.IsEmpty());
    EXPECT_EQ(Reader.Tell(), (int64)Bytes.Num()) << "an older package has no bundle list to read past";
}

TEST(AssetHandles, AnObjectStaysExactlyAsLongAsAHandleHoldsIt)
{
    CDataAsset* Object = NewObject<CDataAsset>();
    const TWeakObjectPtr<CDataAsset> Weak(Object);

    FAssetHandle First = FAssetManager::Get().Load(Object->GetGUID());
    ASSERT_TRUE(First.IsReady()) << "a resident object needs no load";
    EXPECT_EQ(First.Get<CDataAsset>(), Object);
    EXPECT_EQ(First.GetProgress(), 1.0f);

    FAssetHandle Second = FAssetManager::Get().Load(Object->GetGUID());
    FAssetHandle Copy = First;
    First.Reset();
    Copy.Reset();
    EXPECT_TRUE(Weak.IsValid()) << "the second handle still holds it";

    Second.Reset();
    EXPECT_FALSE(Weak.IsValid()) << "the last handle going is the unload";
}

TEST(AssetHandles, OnReadyRunsForWhatIsAlreadyLoaded)
{
    CDataAsset* Object = NewObject<CDataAsset>();
    const FAssetHandle Handle = FAssetManager::Get().Load(Object->GetGUID());

    CObject* Seen = nullptr;
    Handle.OnReady([&Seen](const FAssetHandle& Loaded) { Seen = Loaded.Get(); });
    MainThread::ProcessQueue();
    EXPECT_EQ(Seen, Object);
}

TEST(AssetHandles, WhatResolvesToNothingIsAnInvalidHandleWhoseCallbackStillRuns)
{
    const FAssetHandle ByPath = FAssetManager::Get().Load(FStringView("/Game/Content/NoSuchAsset"));
    const FAssetHandle ById = FAssetManager::Get().Load(SAssetId{ FName("NoSuchGroup"), FName("NoSuchAsset") });
    EXPECT_FALSE(ByPath.IsValid());
    EXPECT_FALSE(ById.IsValid());

    bool bCalled = false;
    ByPath.OnReady([&bCalled](const FAssetHandle& Loaded) { bCalled = Loaded.Get() == nullptr; });
    MainThread::ProcessQueue();
    EXPECT_TRUE(bCalled) << "a failure still completes, so an awaiting caller is never left hanging";
}

TEST(AssetCoroutines, AResidentObjectLoadsWithoutSuspendingAndStaysWhileHeld)
{
    CDataAsset* Object = NewObject<CDataAsset>();
    const TWeakObjectPtr<CDataAsset> Weak(Object);

    TCoroutine<TStrongObjectPtr<CDataAsset>> Load = Coro::LoadObject<CDataAsset>(Object->GetGUID());
    ASSERT_TRUE(Load.IsDone());
    TStrongObjectPtr<CDataAsset> Loaded = Load.GetResult();
    EXPECT_EQ(Loaded.Get(), Object);

    Load = {};
    EXPECT_TRUE(Weak.IsValid()) << "the returned pointer keeps it resident once the load's handle is gone";
    Loaded = nullptr;
    EXPECT_FALSE(Weak.IsValid());
}

TEST(AssetCoroutines, AMissingPathOrTheWrongTypeLoadsNull)
{
    CDataAsset* Object = NewObject<CDataAsset>();

    TCoroutine<TStrongObjectPtr<CDataAsset>> Missing = Coro::LoadObject<CDataAsset>(FStringView("/Game/Content/NoSuchAsset"));
    ASSERT_TRUE(Missing.IsDone());
    EXPECT_EQ(Missing.GetResult().Get(), nullptr);

    TCoroutine<TStrongObjectPtr<CWorld>> WrongType = Coro::LoadObject<CWorld>(Object->GetGUID());
    ASSERT_TRUE(WrongType.IsDone());
    EXPECT_EQ(WrongType.GetResult().Get(), nullptr);

    TCoroutine<TStrongObjectPtr<CObject>> NullPath = Coro::LoadObject(FSoftObjectPath());
    ASSERT_TRUE(NullPath.IsDone());
    EXPECT_EQ(NullPath.GetResult().Get(), nullptr);
}

TEST(AssetCoroutines, AParentAwaitsTheLoadedObject)
{
    CDataAsset* Object = NewObject<CDataAsset>();

    auto Parent = [](FGuid Asset) -> TCoroutine<CDataAsset*>
    {
        TStrongObjectPtr<CDataAsset> Loaded = co_await Coro::LoadObject<CDataAsset>(Asset);
        FAssetHandle Handle = co_await Coro::Load(Asset);
        co_return Handle.Get<CDataAsset>() == Loaded.Get() ? Loaded.Get() : nullptr;
    };

    TCoroutine<CDataAsset*> Run = Parent(Object->GetGUID());
    ASSERT_TRUE(Run.IsDone());
    EXPECT_EQ(Run.GetResult(), Object);
}
