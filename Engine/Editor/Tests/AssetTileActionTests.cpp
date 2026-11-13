#include <gtest/gtest.h>

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Audio/AudioGraph.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Thumbnails/AssetTilePainters.h"

using namespace Lumina;

namespace
{
    struct FFakePreview
    {
        CObject* Started = nullptr;
        bool     bRunning = false;
        int32    Stops = 0;
    };

    FAssetTileAction MakeFakeAction(FFakePreview& Fake)
    {
        return FAssetTileAction
        {
            .Icon = "play",
            .ActiveIcon = "stop",
            .Start = [&Fake](CObject* Asset)
            {
                Fake.Started  = Asset;
                Fake.bRunning = true;
                return FAssetTilePreview
                {
                    .IsRunning = [&Fake] { return Fake.bRunning; },
                    .Stop      = [&Fake] { ++Fake.Stops; },
                };
            },
        };
    }

    FAssetDataPtr CreateStream(const char* Path, const char* Name)
    {
        CAudioStream* Stream = NewObject<CAudioStream>(CPackage::CreatePackage(Path), Name);
        FAssetRegistry::Get().AssetCreated(Stream);
        return FAssetRegistry::Get().GetAssetByGUID(Stream->GetGUID());
    }
}

TEST(AssetTileActions, ADerivedClassFindsTheActionOnItsBase)
{
    CAudioStream::StaticClass();
    CAudioGraph::StaticClass();
    RegisterPendingTypes();

    FFakePreview Fake;
    FAssetTilePainterRegistry Registry;
    Registry.RegisterAction(CSoundBase::StaticClass(), MakeFakeAction(Fake));

    const FAssetTileAction* StreamAction = Registry.FindAction(CAudioStream::StaticClass());
    ASSERT_NE(StreamAction, nullptr);
    EXPECT_EQ(StreamAction, Registry.FindAction(CAudioGraph::StaticClass()));
    EXPECT_EQ(Registry.FindAction(CObject::StaticClass()), nullptr) << "a base never sees its subclass's action";
    EXPECT_EQ(Registry.FindAction(nullptr), nullptr);
}

TEST(AssetTileActions, AClickStartsThePreviewAndItClearsWhenItEnds)
{
    CAudioStream::StaticClass();
    RegisterPendingTypes();

    FFakePreview Fake;
    FAssetTilePainterRegistry Registry;
    Registry.RegisterAction(CSoundBase::StaticClass(), MakeFakeAction(Fake));

    const FAssetDataPtr Asset = CreateStream("/AssetTileActions/Ends", "Ends");
    ASSERT_NE(Asset, nullptr);

    FAssetTilePreviewSlot Slot;
    Slot.Request(*Registry.FindAction(CAudioStream::StaticClass()), *Asset);
    EXPECT_EQ(Fake.Started, FindObject<CObject>(Asset->AssetGUID)) << "a resident asset starts on the click";
    EXPECT_TRUE(Slot.IsActiveFor(Asset->AssetGUID));

    Slot.MarkShown();
    Slot.Tick();
    EXPECT_TRUE(Slot.IsActiveFor(Asset->AssetGUID)) << "a shown preview that still runs keeps going";

    Fake.bRunning = false;
    Slot.MarkShown();
    Slot.Tick();
    EXPECT_FALSE(Slot.IsActiveFor(Asset->AssetGUID)) << "a preview that ended returns the tile to its idle icon";

    FAssetRegistry::Get().AssetDeleted(Asset->AssetGUID);
}

TEST(AssetTileActions, DeletingThePreviewedAssetStopsIt)
{
    CAudioStream::StaticClass();
    RegisterPendingTypes();

    FFakePreview Fake;
    const FAssetTileAction Action = MakeFakeAction(Fake);
    const FAssetDataPtr First  = CreateStream("/AssetTileActions/First", "First");
    const FAssetDataPtr Second = CreateStream("/AssetTileActions/Second", "Second");
    ASSERT_NE(First, nullptr);
    ASSERT_NE(Second, nullptr);

    FAssetTilePreviewSlot Slot;
    Slot.Request(Action, *First);
    Slot.Request(Action, *Second);
    EXPECT_EQ(Fake.Stops, 1) << "starting another preview stops the first";
    EXPECT_FALSE(Slot.IsActiveFor(First->AssetGUID));

    FAssetRegistry::Get().AssetDeleted(Second->AssetGUID);
    Slot.MarkShown();
    Slot.Tick();
    EXPECT_EQ(Fake.Stops, 2);
    EXPECT_FALSE(Slot.IsActiveFor(Second->AssetGUID));

    FAssetRegistry::Get().AssetDeleted(First->AssetGUID);
}

TEST(AssetTileActions, HidingTheTilesStopsThePreview)
{
    CAudioStream::StaticClass();
    RegisterPendingTypes();

    FFakePreview Fake;
    const FAssetDataPtr Asset = CreateStream("/AssetTileActions/Hidden", "Hidden");
    ASSERT_NE(Asset, nullptr);

    FAssetTilePreviewSlot Slot;
    Slot.MarkShown();
    Slot.Request(MakeFakeAction(Fake), *Asset);
    Slot.Tick();
    EXPECT_TRUE(Slot.IsActiveFor(Asset->AssetGUID)) << "the frame that clicked play drew the tiles";

    Slot.Tick();
    EXPECT_EQ(Fake.Stops, 1) << "a closed drawer keeps ticking but no longer draws its tiles";
    EXPECT_FALSE(Slot.IsActiveFor(Asset->AssetGUID));

    FAssetRegistry::Get().AssetDeleted(Asset->AssetGUID);
}
