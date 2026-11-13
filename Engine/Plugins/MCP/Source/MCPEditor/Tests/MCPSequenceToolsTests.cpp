#include "MCPSequenceTools.h"
#include "MCPToolTestUtil.h"

#include "Assets/AssetTypes/Sequence/Sequence.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPSequenceToolsTestsInternal
{
    class MCPSequenceToolsTest : public FRegisteredToolsTest
    {
    protected:

        MCPSequenceToolsTest() : FRegisteredToolsTest("MCPSequenceToolsTests", &RegisterSequenceTools) {}

        void SetUp() override
        {
            RegisterPendingTypes();
            FRegisteredToolsTest::SetUp();
            Sequence = NewObject<CSequence>(CPackage::CreatePackage("/MCPSequenceToolsTests/Scratch"), "Scratch");
            Guid = FString(*Sequence->GetGUID().ToString());
        }

        void AddCamera(const char* Name)
        {
            ExpectSucceeded("sequence.binding", FStringView(Format(R"({{"Sequence":"{}","Name":"{}","Kind":"Camera"}})", Guid, Name)));
        }

        void AddTrack(const char* Type, const char* Binding, const char* Settings = "{}")
        {
            ExpectSucceeded("sequence.track", FStringView(Format(R"({{"Sequence":"{}","Type":"{}","Binding":"{}","Settings":{}}})", Guid, Type, Binding, Settings)));
        }

        void AddCut(const char* Camera, float Start, float End)
        {
            ExpectSucceeded("sequence.add_camera_cut", FStringView(Format(R"({{"Sequence":"{}","Camera":"{}","Start":{},"End":{}}})", Guid, Camera, Start, End)));
        }

        template<typename T>
        TVector<T*> TracksOf()
        {
            TVector<T*> Found;
            for (const TStrongObjectPtr<CSequenceTrack>& Track : Sequence->Tracks)
            {
                if (T* Typed = Cast<T>(Track.Get()))
                {
                    Found.Add(Typed);
                }
            }
            return Found;
        }

        CSequence* Sequence = nullptr;
        FString Guid;
    };
}

using namespace MCPSequenceToolsTestsInternal;

TEST_F(MCPSequenceToolsTest, RemovingAMiddleBindingRenumbersAndDropsWhatPointedAtIt)
{
    AddCamera("A");
    AddCamera("B");
    AddCamera("C");
    AddTrack("Transform", "B");
    AddTrack("Transform", "C");
    AddTrack("LookAt", "A", R"({"TargetBindingIndex":1})");
    AddTrack("LookAt", "A", R"({"TargetBindingIndex":2})");
    AddCut("B", 0.0f, 1.0f);
    AddCut("C", 1.0f, 2.0f);
    ASSERT_EQ(Sequence->Tracks.Num(), 5);

    ExpectSucceeded("sequence.binding", FStringView(Format(R"({{"Sequence":"{}","Action":"Remove","Binding":"B"}})", Guid)));

    ASSERT_EQ(Sequence->Bindings.Num(), 2);
    EXPECT_EQ(Sequence->Bindings[1].Name, FName("C"));

    const TVector<CSequenceTrack_Transform*> Transforms = TracksOf<CSequenceTrack_Transform>();
    ASSERT_EQ(Transforms.Num(), 1);
    EXPECT_EQ(Transforms[0]->BindingIndex, 1);

    const TVector<CSequenceTrack_LookAt*> LookAts = TracksOf<CSequenceTrack_LookAt>();
    ASSERT_EQ(LookAts.Num(), 2);
    EXPECT_EQ(LookAts[0]->BindingIndex, 0);
    EXPECT_EQ(LookAts[0]->TargetBindingIndex, Constants::kIndexNone);
    EXPECT_EQ(LookAts[1]->TargetBindingIndex, 1);

    const TVector<CSequenceTrack_CameraCut*> CutTracks = TracksOf<CSequenceTrack_CameraCut>();
    ASSERT_EQ(CutTracks.Num(), 1);
    ASSERT_EQ(CutTracks[0]->Cuts.Num(), 1);
    EXPECT_EQ(CutTracks[0]->Cuts[0].BindingIndex, 1);
    EXPECT_EQ(CutTracks[0]->Cuts[0].StartTime, 1.0f);
}
