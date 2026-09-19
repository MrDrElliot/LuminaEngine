#include <gtest/gtest.h>

#include "Containers/Name.h"
#include "Core/Math/Math.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ScriptClass.h"
#include "Containers/ContainerOps.h"
#include "Containers/Vector.h"
#include "Core/Templates/Optional.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Paths/Paths.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/ScriptCallback.h"
#include "Scripting/ScriptFunctionMint.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

// Drives the managed FrameMarshal against a real minted frame, where slot and mirror widths have to agree.
namespace
{
    using FReadVector3Fn   = int32(*)(void*, const void*, float*);
    using FWriteVector3Fn  = int32(*)(void*, const void*, float, float, float);
    using FReadByteEnumFn  = int32(*)(void*, const void*);
    using FWriteByteEnumFn = int32(*)(void*, const void*, int32);
    using FReadEntityFn    = int32(*)(void*, const void*, uint32*);
    using FBindMismatchFn  = int32(*)(const void*);
    using FMakeTargetFn    = void*(*)();
    using FFreeTargetFn    = void(*)(void*);
    using FDirectionFn     = uint32(*)(int32);
    using FInvokeFn        = void(*)(void*, const void*, void*);
    using FReadOptionalFn  = int32(*)(void*, const void*, float*);
    using FWriteOptionalFn = int32(*)(void*, const void*, int32, float);
    using FReadVectorFn    = int32(*)(void*, const void*, float*);
    using FAppendVectorFn  = int32(*)(void*, const void*, float);
    using FBindMapFn       = int32(*)(const void*);
    using FResolveKindFn   = int32(*)(int32*);
    using FDescribedFn     = int32(*)(uint8*);
    using FVecIntsFn       = int32(*)(int32, int32*, int32*, int32*);
    using FStringBindFn    = int32(*)(int32, int32*, int32*, int32*);
    using FVecEntitiesFn   = int32(*)(int32, uint32*);
    using FVecStructsFn    = int32(*)(int32, float*);
    using FBindCallbackFn  = uint64(*)();
    using FCallbackResFn   = uint64(*)(int32*);
    using FBindWrapperFn   = void*(*)(void*);
    using FProbeWrapperFn  = int32(*)(void*, int32*);
    using FFreeWrapperFn   = void(*)(void*);
    using FHasInvokerFn    = int32(*)(int32);

    Scripting::FScriptExportField ParamField(const char* Name, EPropertyTypeFlags Kind, EPropertyFlags Direction)
    {
        Scripting::FScriptExportField Field;
        Field.Name = FName(Name);
        Field.Type = MakeShared<Scripting::FScriptExportType>();
        Field.Type->Kind = Kind;
        Field.Flags = (uint32)Direction;
        return Field;
    }

    Scripting::FScriptExportField ScalarField(const char* Name, EPropertyTypeFlags Kind)
    {
        Scripting::FScriptExportField Field;
        Field.Name = FName(Name);
        Field.Type = MakeShared<Scripting::FScriptExportType>();
        Field.Type->Kind = Kind;
        return Field;
    }

    Scripting::FScriptExportField EntityField(const char* Name)
    {
        Scripting::FScriptExportField Field = ScalarField(Name, EPropertyTypeFlags::UInt32);
        Field.Type->bEntity = true;
        return Field;
    }

    Scripting::FScriptExportField ByteEnumField(const char* Name, const char* EnumName)
    {
        Scripting::FScriptExportField Field;
        Field.Name = FName(Name);
        Field.Type = MakeShared<Scripting::FScriptExportType>();
        Field.Type->Kind = EPropertyTypeFlags::Enum;
        Field.Type->EnumName = FName(EnumName);
        Field.Type->EnumUnderlying = EPropertyTypeFlags::UInt8;
        Field.Type->EnumEntries.push_back({FName("Zero"), 0});
        Field.Type->EnumEntries.push_back({FName("One"), 1});
        Field.Type->EnumEntries.push_back({FName("Two"), 2});
        Field.Type->EnumEntries.push_back({FName("Highest"), 255});
        return Field;
    }

    Scripting::FScriptExportField VectorField(const char* Name, EPropertyTypeFlags Element)
    {
        Scripting::FScriptExportField Field;
        Field.Name = FName(Name);
        Field.Type = MakeShared<Scripting::FScriptExportType>();
        Field.Type->Kind = EPropertyTypeFlags::Vector;
        Field.Type->ElementType = MakeShared<Scripting::FScriptExportType>();
        Field.Type->ElementType->Kind = Element;
        return Field;
    }

    Scripting::FScriptExportField OptionalField(const char* Name, EPropertyTypeFlags Payload)
    {
        Scripting::FScriptExportField Field;
        Field.Name = FName(Name);
        Field.Type = MakeShared<Scripting::FScriptExportType>();
        Field.Type->Kind = EPropertyTypeFlags::Optional;
        Field.Type->ElementType = MakeShared<Scripting::FScriptExportType>();
        Field.Type->ElementType->Kind = Payload;
        return Field;
    }

    Scripting::FScriptExportField NativeStructField(const char* Name, const char* NativeType)
    {
        Scripting::FScriptExportField Field;
        Field.Name = FName(Name);
        Field.Type = MakeShared<Scripting::FScriptExportType>();
        Field.Type->Kind = EPropertyTypeFlags::Struct;
        Field.Type->NativeName = FName(NativeType);
        return Field;
    }

    void NoOpThunk(const FFunction&, void*, void*)
    {
    }

    FFunction* MintFrame(const char* ClassName, const Scripting::FScriptExportSchema& ParamSchema,
        const char* FunctionName = "Marshal", int32 ReturnIndex = -1)
    {
        static const bool bReflectionReady = [] { ProcessNewlyLoadedCObjects(); return true; }();
        (void)bReflectionReady;

        CScriptClass* Class = FScriptableRegistry::Mint(ClassName, "CScriptableTest");
        if (Class == nullptr)
        {
            return nullptr;
        }

        Scripting::FScriptExportSchema Properties;
        Properties.Fields.push_back(ScalarField("Anchor", EPropertyTypeFlags::Int32));
        Scripting::AppendScriptPropertiesToClass(Class, Properties);

        CScriptStruct* Record = Cast<CScriptStruct>(Class->LayoutRecord.Get());
        if (Record == nullptr)
        {
            return nullptr;
        }

        FFunction* Function = Scripting::MintScriptFunction(*Class, *Record, FName(FunctionName),
            ParamSchema, ReturnIndex, &NoOpThunk);

        Class->GetDefaultObject();
        return Function;
    }
}

class FFrameMarshalTest : public ::testing::Test
{
public:

    static void SetUpTestSuite()
    {
        Paths::InitializePaths();
        DotNet::Initialize();
    }

    static void TearDownTestSuite()
    {
        if (DotNet::IsInitialized())
        {
            DotNet::Shutdown();
        }
    }

    void SetUp() override
    {
        if (!DotNet::IsInitialized())
        {
            GTEST_SKIP() << "the managed host did not start in this process";
        }
    }
};

TEST_F(FFrameMarshalTest, AStructMirrorIsReadFromItsFrameSlot)
{
    auto* Read = (FReadVector3Fn)DotNet::ResolveManagedExport("Test_FrameReadVector3");
    ASSERT_NE(Read, nullptr);

    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(NativeStructField("Where", "FVector3"));

    FFunction* Function = MintFrame("FrameMarshal_ReadVector", Params);
    ASSERT_NE(Function, nullptr);

    FProperty* Slot = Function->GetParams()[0];

    FFunctionFrame Frame(*Function);
    *Slot->GetValuePtr<FVector3>(Frame.GetMemory()) = FVector3(1.0f, 2.0f, 3.0f);

    float Managed[3] = {0.0f, 0.0f, 0.0f};
    ASSERT_EQ(Read(Frame.GetMemory(), Slot, Managed), 1) << "the mirror was refused by the binder";

    EXPECT_FLOAT_EQ(Managed[0], 1.0f);
    EXPECT_FLOAT_EQ(Managed[1], 2.0f);
    EXPECT_FLOAT_EQ(Managed[2], 3.0f);
}

TEST_F(FFrameMarshalTest, AStructMirrorIsWrittenBackIntoItsFrameSlot)
{
    auto* Write = (FWriteVector3Fn)DotNet::ResolveManagedExport("Test_FrameWriteVector3");
    ASSERT_NE(Write, nullptr);

    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(NativeStructField("Where", "FVector3"));

    FFunction* Function = MintFrame("FrameMarshal_WriteVector", Params);
    ASSERT_NE(Function, nullptr);

    FProperty* Slot = Function->GetParams()[0];

    FFunctionFrame Frame(*Function);
    ASSERT_EQ(Write(Frame.GetMemory(), Slot, 4.0f, 5.0f, 6.0f), 1);

    const FVector3& Value = *Slot->GetValuePtr<FVector3>(Frame.GetMemory());
    EXPECT_FLOAT_EQ(Value.x, 4.0f);
    EXPECT_FLOAT_EQ(Value.y, 5.0f);
    EXPECT_FLOAT_EQ(Value.z, 6.0f);
}

TEST_F(FFrameMarshalTest, AByteEnumIsReadAtItsOwnWidth)
{
    auto* Read = (FReadByteEnumFn)DotNet::ResolveManagedExport("Test_FrameReadByteEnum");
    ASSERT_NE(Read, nullptr);

    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(ByteEnumField("Channel", "FrameMarshal_Channel"));
    Params.Fields.push_back(ScalarField("After", EPropertyTypeFlags::UInt8));

    FFunction* Function = MintFrame("FrameMarshal_ReadEnum", Params);
    ASSERT_NE(Function, nullptr);

    FProperty* Channel = Function->GetParams()[0];
    FProperty* After   = Function->GetParams()[1];

    ASSERT_EQ(After->Offset, Channel->Offset + 1u) << "the neighbour has to abut the enum to be a witness";

    FFunctionFrame Frame(*Function);
    *Channel->GetValuePtr<uint8>(Frame.GetMemory()) = 2;
    *After->GetValuePtr<uint8>(Frame.GetMemory()) = 0xFFu;

    EXPECT_EQ(Read(Frame.GetMemory(), Channel), 2) << "the read took in the neighbouring byte";
}

// A four-byte write into a one-byte slot lands on whatever follows, which is the whole point of this one.
TEST_F(FFrameMarshalTest, AByteEnumWriteStaysInsideItsSlot)
{
    auto* Write = (FWriteByteEnumFn)DotNet::ResolveManagedExport("Test_FrameWriteByteEnum");
    ASSERT_NE(Write, nullptr);

    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(ByteEnumField("Channel", "FrameMarshal_WriteChannel"));
    Params.Fields.push_back(ScalarField("After", EPropertyTypeFlags::UInt8));

    FFunction* Function = MintFrame("FrameMarshal_WriteEnum", Params);
    ASSERT_NE(Function, nullptr);

    FProperty* Channel = Function->GetParams()[0];
    FProperty* After   = Function->GetParams()[1];

    ASSERT_EQ(After->Offset, Channel->Offset + 1u) << "the neighbour has to abut the enum to be a witness";

    FFunctionFrame Frame(*Function);
    *After->GetValuePtr<uint8>(Frame.GetMemory()) = 0xABu;

    ASSERT_EQ(Write(Frame.GetMemory(), Channel, 255), 1);

    EXPECT_EQ(*Channel->GetValuePtr<uint8>(Frame.GetMemory()), 255u);
    EXPECT_EQ(*After->GetValuePtr<uint8>(Frame.GetMemory()), 0xABu) << "the write reached past its slot";
}

TEST_F(FFrameMarshalTest, AnEntityHandleCrossesAsItsPackedId)
{
    auto* Read = (FReadEntityFn)DotNet::ResolveManagedExport("Test_FrameReadEntity");
    ASSERT_NE(Read, nullptr);

    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(EntityField("Who"));

    FFunction* Function = MintFrame("FrameMarshal_ReadEntity", Params);
    ASSERT_NE(Function, nullptr);

    FProperty* Slot = Function->GetParams()[0];

    FFunctionFrame Frame(*Function);
    *Slot->GetValuePtr<uint32>(Frame.GetMemory()) = 0x00123456u;

    uint32 Managed = 0;
    ASSERT_EQ(Read(Frame.GetMemory(), Slot, &Managed), 1);
    EXPECT_EQ(Managed, 0x00123456u);
}

TEST_F(FFrameMarshalTest, ASlotTooNarrowForItsMirrorIsRefused)
{
    auto* Bind = (FBindMismatchFn)DotNet::ResolveManagedExport("Test_FrameBindMismatchedWidth");
    ASSERT_NE(Bind, nullptr);

    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(ScalarField("NotAVector", EPropertyTypeFlags::Float));

    FFunction* Function = MintFrame("FrameMarshal_Mismatch", Params);
    ASSERT_NE(Function, nullptr);

    EXPECT_EQ(Bind(Function->GetParams()[0]), 0) << "a 4-byte slot accepted a 12-byte mirror";
}

// Both sides look a reflected function up by name, so an overload is refused where it is declared.
TEST_F(FFrameMarshalTest, AnOverloadedScriptFunctionIsRefusedWhereItIsDeclared)
{
    auto* Described = (FDescribedFn)DotNet::ResolveManagedExport("Test_DescribedFunctionCount");
    ASSERT_NE(Described, nullptr);

    uint8 bAmbiguousDescribed = 1;
    const int32 Count = Described(&bAmbiguousDescribed);

    EXPECT_EQ(bAmbiguousDescribed, 0) << "an overloaded name was described and would mint twice";
    EXPECT_EQ(Count, 1) << "the distinctly named function should still be described";
}

TEST_F(FFrameMarshalTest, AParameterCarriesItsDirectionOntoTheMintedProperty)
{
    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(ParamField("In", EPropertyTypeFlags::Int32, EPropertyFlags::None));
    Params.Fields.push_back(ParamField("Out", EPropertyTypeFlags::Int32, EPropertyFlags::OutParam));
    Params.Fields.push_back(ParamField("Both", EPropertyTypeFlags::Int32,
        EPropertyFlags::OutParam | EPropertyFlags::RefParam));

    FFunction* Function = MintFrame("FrameMarshal_Direction", Params);
    ASSERT_NE(Function, nullptr);

    const TSpan<FProperty* const> All = Function->GetParams();
    ASSERT_EQ(All.size(), 3u);

    EXPECT_FALSE(All[0]->IsOutParam());
    EXPECT_TRUE(All[1]->IsOutParam());
    EXPECT_FALSE(All[1]->IsRefParam()) << "an out parameter does not carry its incoming value";
    EXPECT_TRUE(All[2]->IsOutParam());
    EXPECT_TRUE(All[2]->IsRefParam());
}

// Pins out, ref and in against what the C# compiler emits rather than what this layer assumes.
TEST_F(FFrameMarshalTest, ManagedParameterDirectionsAreClassifiedFromTheSignature)
{
    auto* Direction = (FDirectionFn)DotNet::ResolveManagedExport("Test_ParameterDirection");
    ASSERT_NE(Direction, nullptr);

    const uint32 Out = (uint32)EPropertyFlags::OutParam;
    const uint32 Ref = (uint32)(EPropertyFlags::OutParam | EPropertyFlags::RefParam);

    EXPECT_EQ(Direction(0), 0u) << "a plain parameter is an input";
    EXPECT_EQ(Direction(1), Out);
    EXPECT_EQ(Direction(2), Ref);
    EXPECT_EQ(Direction(3), 0u) << "C# in is by-ref but read-only";
}

TEST_F(FFrameMarshalTest, AnOutParameterComesBackThroughTheFrame)
{
    auto* MakeTarget = (FMakeTargetFn)DotNet::ResolveManagedExport("Test_MakeMarshalTarget");
    auto* FreeTarget = (FFreeTargetFn)DotNet::ResolveManagedExport("Test_FreeMarshalTarget");
    auto* Invoke     = (FInvokeFn)DotNet::ResolveManagedExport("InvokeScriptFunction");
    ASSERT_NE(MakeTarget, nullptr);
    ASSERT_NE(Invoke, nullptr);

    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(ParamField("In", EPropertyTypeFlags::Int32, EPropertyFlags::None));
    Params.Fields.push_back(ParamField("Out", EPropertyTypeFlags::Int32, EPropertyFlags::OutParam));

    FFunction* Function = MintFrame("FrameMarshal_Out", Params, "MarshalOut");
    ASSERT_NE(Function, nullptr);

    void* Target = MakeTarget();
    ASSERT_NE(Target, nullptr);

    FFunctionFrame Frame(*Function);
    Frame.At<int32>(0) = 21;
    Frame.At<int32>(1) = 0;

    Invoke(Target, Function, Frame.GetMemory());

    EXPECT_EQ(Frame.At<int32>(1), 42) << "the out argument was not written back into the frame";

    FreeTarget(Target);
}

TEST_F(FFrameMarshalTest, ARefParameterIsReadInAndWrittenBack)
{
    auto* MakeTarget = (FMakeTargetFn)DotNet::ResolveManagedExport("Test_MakeMarshalTarget");
    auto* FreeTarget = (FFreeTargetFn)DotNet::ResolveManagedExport("Test_FreeMarshalTarget");
    auto* Invoke     = (FInvokeFn)DotNet::ResolveManagedExport("InvokeScriptFunction");
    ASSERT_NE(MakeTarget, nullptr);
    ASSERT_NE(Invoke, nullptr);

    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(ParamField("Value", EPropertyTypeFlags::Int32,
        EPropertyFlags::OutParam | EPropertyFlags::RefParam));

    FFunction* Function = MintFrame("FrameMarshal_Ref", Params, "MarshalRef");
    ASSERT_NE(Function, nullptr);

    void* Target = MakeTarget();
    ASSERT_NE(Target, nullptr);

    FFunctionFrame Frame(*Function);
    Frame.At<int32>(0) = 10;

    Invoke(Target, Function, Frame.GetMemory());

    EXPECT_EQ(Frame.At<int32>(0), 15) << "a ref argument has to carry its incoming value in and the new one out";

    FreeTarget(Target);
}

// A reflected TOptional<float>, whose ops the Reflector baked, is the only real optional slot to bind against.
namespace
{
    FFunction* FindOptionalEcho()
    {
        static const bool bReflectionReady = [] { ProcessNewlyLoadedCObjects(); return true; }();
        (void)bReflectionReady;

        return CScriptableTest::StaticClass()->FindFunction(FName("OnEchoOptional"));
    }
}

TEST_F(FFrameMarshalTest, AnEngagedOptionalCrossesAsItsPayload)
{
    auto* Read = (FReadOptionalFn)DotNet::ResolveManagedExport("Test_FrameReadOptional");
    ASSERT_NE(Read, nullptr);

    FFunction* Function = FindOptionalEcho();
    ASSERT_NE(Function, nullptr);

    FProperty* Slot = Function->GetParams()[0];
    ASSERT_EQ(Slot->GetType(), EPropertyTypeFlags::Optional);

    FFunctionFrame Frame(*Function);
    Frame.At<TOptional<float>>(0) = 7.5f;

    float Payload = 0.0f;
    EXPECT_EQ(Read(Frame.GetMemory(), Slot, &Payload), 1);
    EXPECT_FLOAT_EQ(Payload, 7.5f);
}

TEST_F(FFrameMarshalTest, AnUnsetOptionalCrossesAsNull)
{
    auto* Read = (FReadOptionalFn)DotNet::ResolveManagedExport("Test_FrameReadOptional");
    ASSERT_NE(Read, nullptr);

    FFunction* Function = FindOptionalEcho();
    ASSERT_NE(Function, nullptr);

    FProperty* Slot = Function->GetParams()[0];

    FFunctionFrame Frame(*Function);
    Frame.At<TOptional<float>>(0).Reset();

    float Payload = -1.0f;
    EXPECT_EQ(Read(Frame.GetMemory(), Slot, &Payload), 0);
    EXPECT_FLOAT_EQ(Payload, -1.0f) << "nothing should have been written for an unset optional";
}

TEST_F(FFrameMarshalTest, WritingAnOptionalEngagesTheFrameSlot)
{
    auto* Write = (FWriteOptionalFn)DotNet::ResolveManagedExport("Test_FrameWriteOptional");
    ASSERT_NE(Write, nullptr);

    FFunction* Function = FindOptionalEcho();
    ASSERT_NE(Function, nullptr);

    FProperty* Slot = Function->GetParams()[0];

    FFunctionFrame Frame(*Function);
    Frame.At<TOptional<float>>(0).Reset();

    ASSERT_EQ(Write(Frame.GetMemory(), Slot, 1, 3.25f), 1);

    EXPECT_TRUE(Frame.At<TOptional<float>>(0).IsSet());
    EXPECT_FLOAT_EQ(Frame.At<TOptional<float>>(0).GetValue(), 3.25f);
}

TEST_F(FFrameMarshalTest, WritingANullOptionalResetsTheFrameSlot)
{
    auto* Write = (FWriteOptionalFn)DotNet::ResolveManagedExport("Test_FrameWriteOptional");
    ASSERT_NE(Write, nullptr);

    FFunction* Function = FindOptionalEcho();
    ASSERT_NE(Function, nullptr);

    FProperty* Slot = Function->GetParams()[0];

    FFunctionFrame Frame(*Function);
    Frame.At<TOptional<float>>(0) = 11.0f;

    ASSERT_EQ(Write(Frame.GetMemory(), Slot, 0, 0.0f), 1);

    EXPECT_FALSE(Frame.At<TOptional<float>>(0).IsSet());
}

TEST_F(FFrameMarshalTest, AnOptionalRoundTripsThroughTheDispatcher)
{
    auto* MakeTarget = (FMakeTargetFn)DotNet::ResolveManagedExport("Test_MakeMarshalTarget");
    auto* FreeTarget = (FFreeTargetFn)DotNet::ResolveManagedExport("Test_FreeMarshalTarget");
    auto* Invoke     = (FInvokeFn)DotNet::ResolveManagedExport("InvokeScriptFunction");
    ASSERT_NE(MakeTarget, nullptr);
    ASSERT_NE(Invoke, nullptr);

    FFunction* Function = FindOptionalEcho();
    ASSERT_NE(Function, nullptr);
    ASSERT_TRUE(Function->HasReturn());

    void* Target = MakeTarget();
    ASSERT_NE(Target, nullptr);

    {
        FFunctionFrame Frame(*Function);
        Frame.At<TOptional<float>>(0) = 4.0f;

        Invoke(Target, Function, Frame.GetMemory());

        ASSERT_TRUE(Frame.Return<TOptional<float>>().IsSet());
        EXPECT_FLOAT_EQ(Frame.Return<TOptional<float>>().GetValue(), 8.0f);
    }

    {
        FFunctionFrame Frame(*Function);
        Frame.At<TOptional<float>>(0).Reset();
        Frame.Return<TOptional<float>>() = 99.0f;

        Invoke(Target, Function, Frame.GetMemory());

        EXPECT_FALSE(Frame.Return<TOptional<float>>().IsSet()) << "a null return has to reset the slot";
    }

    FreeTarget(Target);
}

// A reflected TVector<float>, so the frame slot is a real vector the view can alias through its ops table.
namespace
{
    FFunction* FindVectorAppend()
    {
        static const bool bReflectionReady = [] { ProcessNewlyLoadedCObjects(); return true; }();
        (void)bReflectionReady;

        return CScriptableTest::StaticClass()->FindFunction(FName("OnAppendSum"));
    }
}

TEST_F(FFrameMarshalTest, AContainerSlotIsReadThroughAViewOverTheFramesStorage)
{
    auto* Read = (FReadVectorFn)DotNet::ResolveManagedExport("Test_FrameReadVectorCount");
    ASSERT_NE(Read, nullptr);

    FFunction* Function = FindVectorAppend();
    ASSERT_NE(Function, nullptr);

    FProperty* Slot = Function->GetParams()[0];
    ASSERT_EQ(Slot->GetType(), EPropertyTypeFlags::Vector);

    FFunctionFrame Frame(*Function);
    TVector<float>& Values = Frame.At<TVector<float>>(0);
    Values.push_back(1.0f);
    Values.push_back(2.0f);
    Values.push_back(4.0f);

    float Sum = 0.0f;
    EXPECT_EQ(Read(Frame.GetMemory(), Slot, &Sum), 3) << "the view did not see the frame's elements";
    EXPECT_FLOAT_EQ(Sum, 7.0f);
}

TEST_F(FFrameMarshalTest, AppendingThroughTheViewGrowsTheFramesContainer)
{
    auto* Append = (FAppendVectorFn)DotNet::ResolveManagedExport("Test_FrameAppendToVector");
    ASSERT_NE(Append, nullptr);

    FFunction* Function = FindVectorAppend();
    ASSERT_NE(Function, nullptr);

    FProperty* Slot = Function->GetParams()[0];

    FFunctionFrame Frame(*Function);
    TVector<float>& Values = Frame.At<TVector<float>>(0);
    Values.push_back(9.0f);

    ASSERT_EQ(Append(Frame.GetMemory(), Slot, 5.5f), 1);

    ASSERT_EQ(Values.size(), 2u) << "the append went somewhere other than the frame's own vector";
    EXPECT_FLOAT_EQ(Values[0], 9.0f);
    EXPECT_FLOAT_EQ(Values[1], 5.5f);
}

// The two ops tables are not interchangeable, so the kind check has to refuse rather than reinterpret.
TEST_F(FFrameMarshalTest, AMapViewIsRefusedOnAVectorSlot)
{
    auto* Bind = (FBindMapFn)DotNet::ResolveManagedExport("Test_FrameBindMapOverVector");
    ASSERT_NE(Bind, nullptr);

    FFunction* Function = FindVectorAppend();
    ASSERT_NE(Function, nullptr);

    EXPECT_EQ(Bind(Function->GetParams()[0]), 0);
}

TEST_F(FFrameMarshalTest, AContainerRoundTripsThroughTheDispatcher)
{
    auto* MakeTarget = (FMakeTargetFn)DotNet::ResolveManagedExport("Test_MakeMarshalTarget");
    auto* FreeTarget = (FFreeTargetFn)DotNet::ResolveManagedExport("Test_FreeMarshalTarget");
    auto* Invoke     = (FInvokeFn)DotNet::ResolveManagedExport("InvokeScriptFunction");
    ASSERT_NE(MakeTarget, nullptr);
    ASSERT_NE(Invoke, nullptr);

    FFunction* Function = FindVectorAppend();
    ASSERT_NE(Function, nullptr);

    void* Target = MakeTarget();
    ASSERT_NE(Target, nullptr);

    FFunctionFrame Frame(*Function);
    TVector<float>& Values = Frame.At<TVector<float>>(0);
    Values.push_back(1.0f);
    Values.push_back(2.0f);
    Values.push_back(3.0f);

    Invoke(Target, Function, Frame.GetMemory());

    ASSERT_EQ(Values.size(), 4u) << "the callee's append did not reach the caller's vector";
    EXPECT_FLOAT_EQ(Values[3], 6.0f);

    FreeTarget(Target);
}

// A script-minted array keeps its elements in a byte vector, so the view has to read its length from the ops
// table rather than the header it shares with a TVector.
TEST_F(FFrameMarshalTest, AScriptMintedArrayIsReadThroughTheSameView)
{
    auto* Read = (FReadVectorFn)DotNet::ResolveManagedExport("Test_FrameReadVectorCount");
    auto* Append = (FAppendVectorFn)DotNet::ResolveManagedExport("Test_FrameAppendToVector");
    ASSERT_NE(Read, nullptr);
    ASSERT_NE(Append, nullptr);

    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(VectorField("Values", EPropertyTypeFlags::Float));

    FFunction* Function = MintFrame("FrameMarshal_ScriptArray", Params);
    ASSERT_NE(Function, nullptr);

    FProperty* Slot = Function->GetParams()[0];
    ASSERT_EQ(Slot->GetType(), EPropertyTypeFlags::Vector);

    const FVectorOps* Ops = static_cast<FArrayProperty*>(Slot)->GetOps();
    ASSERT_NE(Ops, nullptr);

    FFunctionFrame Frame(*Function);
    void* Vector = Slot->GetValuePtr<void>(Frame.GetMemory());

    for (float Value : {2.0f, 3.0f, 5.0f})
    {
        Ops->PushBack(Vector, &Value);
    }
    ASSERT_EQ(Ops->Size(Vector), 3u);

    float Sum = 0.0f;
    EXPECT_EQ(Read(Frame.GetMemory(), Slot, &Sum), 3) << "the view mis-read the script array's length";
    EXPECT_FLOAT_EQ(Sum, 10.0f);

    ASSERT_EQ(Append(Frame.GetMemory(), Slot, 7.0f), 1);
    EXPECT_EQ(Ops->Size(Vector), 4u);
}

// A script-declared optional has no C++ TOptional<T> to take ops from, so the mint synthesizes them and the
// managed side binds it exactly as it binds a native one.
TEST_F(FFrameMarshalTest, AScriptMintedOptionalBindsAsANullable)
{
    auto* Read = (FReadOptionalFn)DotNet::ResolveManagedExport("Test_FrameReadOptional");
    auto* Write = (FWriteOptionalFn)DotNet::ResolveManagedExport("Test_FrameWriteOptional");
    ASSERT_NE(Read, nullptr);
    ASSERT_NE(Write, nullptr);

    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(OptionalField("Maybe", EPropertyTypeFlags::Float));

    FFunction* Function = MintFrame("FrameMarshal_ScriptOptional", Params);
    ASSERT_NE(Function, nullptr);

    FProperty* Slot = Function->GetParams()[0];
    ASSERT_EQ(Slot->GetType(), EPropertyTypeFlags::Optional);

    FOptionalProperty* Optional = static_cast<FOptionalProperty*>(Slot);
    ASSERT_NE(Optional->GetInternalProperty(), nullptr);
    EXPECT_EQ(Optional->GetInternalProperty()->GetElementSize(), sizeof(float));

    FFunctionFrame Frame(*Function);
    void* Member = Slot->GetValuePtr<void>(Frame.GetMemory());

    float Payload = -1.0f;
    EXPECT_EQ(Read(Frame.GetMemory(), Slot, &Payload), 0) << "a fresh optional has to come up unset";

    const float Engaged = 7.5f;
    Optional->SetValue(Member, &Engaged);
    EXPECT_EQ(Read(Frame.GetMemory(), Slot, &Payload), 1);
    EXPECT_FLOAT_EQ(Payload, 7.5f);

    ASSERT_EQ(Write(Frame.GetMemory(), Slot, 1, 3.25f), 1);
    ASSERT_TRUE(Optional->HasValue(Member));
    EXPECT_FLOAT_EQ(*static_cast<const float*>(Optional->GetValue(Member)), 3.25f);

    ASSERT_EQ(Write(Frame.GetMemory(), Slot, 0, 0.0f), 1);
    EXPECT_FALSE(Optional->HasValue(Member)) << "a null nullable has to reset the script optional";
}

// The synthesized ops are what FOptionalProperty's own serialize, copy and compare run on, so they have to
// hold up away from the managed side too.
TEST_F(FFrameMarshalTest, AScriptMintedOptionalCopiesAndComparesThroughItsOps)
{
    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(OptionalField("A", EPropertyTypeFlags::Int32));
    Params.Fields.push_back(OptionalField("B", EPropertyTypeFlags::Int32));

    FFunction* Function = MintFrame("FrameMarshal_ScriptOptionalOps", Params);
    ASSERT_NE(Function, nullptr);

    FOptionalProperty* First  = static_cast<FOptionalProperty*>(Function->GetParams()[0]);
    FOptionalProperty* Second = static_cast<FOptionalProperty*>(Function->GetParams()[1]);

    FFunctionFrame Frame(*Function);
    void* A = First->GetValuePtr<void>(Frame.GetMemory());
    void* B = Second->GetValuePtr<void>(Frame.GetMemory());

    EXPECT_TRUE(First->Identical(A, B)) << "two unset optionals are equal";

    const int32 Value = 41;
    First->SetValue(A, &Value);
    EXPECT_FALSE(First->Identical(A, B));

    First->CopyCompleteValue(B, A);
    ASSERT_TRUE(Second->HasValue(B));
    EXPECT_EQ(*static_cast<const int32*>(Second->GetValue(B)), 41);
    EXPECT_TRUE(First->Identical(A, B));

    First->Reset(A);
    EXPECT_FALSE(First->HasValue(A));
    EXPECT_TRUE(Second->HasValue(B)) << "the copy owns its own payload";
}

TEST_F(FFrameMarshalTest, ANullableResolvesToAnOptionalOverItsPayload)
{
    auto* Resolve = (FResolveKindFn)DotNet::ResolveManagedExport("Test_ResolveNullableKind");
    ASSERT_NE(Resolve, nullptr);

    int32 PayloadKind = -1;
    EXPECT_EQ(Resolve(&PayloadKind), (int32)EPropertyTypeFlags::Optional);
    EXPECT_EQ(PayloadKind, (int32)EPropertyTypeFlags::Float);
}

TEST_F(FFrameMarshalTest, AScriptMintedOptionalRoundTripsThroughTheDispatcher)
{
    auto* MakeTarget = (FMakeTargetFn)DotNet::ResolveManagedExport("Test_MakeMarshalTarget");
    auto* FreeTarget = (FFreeTargetFn)DotNet::ResolveManagedExport("Test_FreeMarshalTarget");
    auto* Invoke     = (FInvokeFn)DotNet::ResolveManagedExport("InvokeScriptFunction");
    ASSERT_NE(MakeTarget, nullptr);
    ASSERT_NE(Invoke, nullptr);

    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(OptionalField("In", EPropertyTypeFlags::Float));
    Params.Fields.push_back(OptionalField("ReturnValue", EPropertyTypeFlags::Float));

    FFunction* Function = MintFrame("FrameMarshal_ScriptOptionalCall", Params, "OnMaybeDouble", 1);
    ASSERT_NE(Function, nullptr);
    ASSERT_TRUE(Function->HasReturn());

    FOptionalProperty* In     = static_cast<FOptionalProperty*>(Function->GetParams()[0]);
    FOptionalProperty* Return = static_cast<FOptionalProperty*>(Function->GetReturnParam());

    void* Target = MakeTarget();
    ASSERT_NE(Target, nullptr);

    {
        FFunctionFrame Frame(*Function);
        const float Value = 2.0f;
        In->SetValue(In->GetValuePtr<void>(Frame.GetMemory()), &Value);

        Invoke(Target, Function, Frame.GetMemory());

        void* Slot = Return->GetValuePtr<void>(Frame.GetMemory());
        ASSERT_TRUE(Return->HasValue(Slot));
        EXPECT_FLOAT_EQ(*static_cast<const float*>(Return->GetValue(Slot)), 2.5f);
    }

    {
        FFunctionFrame Frame(*Function);
        const float Seed = 99.0f;
        Return->SetValue(Return->GetValuePtr<void>(Frame.GetMemory()), &Seed);

        Invoke(Target, Function, Frame.GetMemory());

        EXPECT_FALSE(Return->HasValue(Return->GetValuePtr<void>(Frame.GetMemory())))
            << "a null return has to reset the script optional";
    }

    FreeTarget(Target);
}

// The out container binding replaces a hand-written two-pass buffer export, so what it hands back has to be
// the whole vector, whatever its element.
TEST_F(FFrameMarshalTest, AReflectedOutContainerCrossesAsAnArray)
{
    auto* Ints = (FVecIntsFn)DotNet::ResolveManagedExport("Test_VectorBindingInts");
    ASSERT_NE(Ints, nullptr);

    int32 First = -1;
    int32 Last = -1;
    int32 Calls = 0;
    EXPECT_EQ(Ints(8, &First, &Last, &Calls), 8);
    EXPECT_EQ(First, 0);
    EXPECT_EQ(Last, 7);
    EXPECT_EQ(Calls, 1) << "a result that fits the scratch buffer should cost one crossing";

    EXPECT_EQ(Ints(0, &First, &Last, &Calls), 0) << "an empty container is an empty array, not a null one";
}

// Past the stack scratch the binding sizes an exact buffer and refills it, which is the only path that
// calls the bound function twice.
TEST_F(FFrameMarshalTest, AnOutContainerLargerThanTheScratchBufferStillArrivesWhole)
{
    auto* Ints = (FVecIntsFn)DotNet::ResolveManagedExport("Test_VectorBindingInts");
    ASSERT_NE(Ints, nullptr);

    // The scratch holds 1024 bytes, so this overflows it for a 4-byte element.
    const int32 Count = 900;
    int32 First = -1;
    int32 Last = -1;
    int32 Calls = 0;
    EXPECT_EQ(Ints(Count, &First, &Last, &Calls), Count);
    EXPECT_EQ(First, 0);
    EXPECT_EQ(Last, Count - 1);
    EXPECT_EQ(Calls, 2) << "an overflow refills an exact buffer, so the query runs a second time";
}

// A string return sizes into stack scratch first, so an ordinary name costs one crossing rather than two.
TEST_F(FFrameMarshalTest, AStringReturnThatFitsTheScratchBufferCostsOneCrossing)
{
    auto* Bind = (FStringBindFn)DotNet::ResolveManagedExport("Test_StringBinding");
    ASSERT_NE(Bind, nullptr);

    int32 Calls = 0;
    int32 First = -1;
    int32 Last = -1;
    const int32 Length = 32;
    EXPECT_EQ(Bind(Length, &Calls, &First, &Last), Length);
    EXPECT_EQ(Calls, 1) << "a string that fits the scratch buffer should cost one crossing";
    EXPECT_EQ(First, 'a');
    EXPECT_EQ(Last, 'a' + ((Length - 1) % 26));
}

// Past the scratch the binding sizes an exact buffer and refills it, which is the only path that calls twice.
TEST_F(FFrameMarshalTest, AStringReturnLargerThanTheScratchBufferStillArrivesWhole)
{
    auto* Bind = (FStringBindFn)DotNet::ResolveManagedExport("Test_StringBinding");
    ASSERT_NE(Bind, nullptr);

    int32 Calls = 0;
    int32 First = -1;
    int32 Last = -1;
    const int32 Length = 900;
    EXPECT_EQ(Bind(Length, &Calls, &First, &Last), Length);
    EXPECT_EQ(Calls, 2) << "an overflow refills an exact buffer, so the query runs a second time";
    EXPECT_EQ(First, 'a');
    EXPECT_EQ(Last, 'a' + ((Length - 1) % 26));
}

TEST_F(FFrameMarshalTest, AnOutContainerOfEntitiesKeepsItsPackedIds)
{
    auto* Entities = (FVecEntitiesFn)DotNet::ResolveManagedExport("Test_VectorBindingEntities");
    ASSERT_NE(Entities, nullptr);

    uint32 Last = 0;
    EXPECT_EQ(Entities(5, &Last), 5);
    EXPECT_EQ(Last, (uint32)ECS::FEntity::FromPacked(4));
}

// The element stride is the native struct's, not the managed handle's, so a wide element has to survive.
TEST_F(FFrameMarshalTest, AnOutContainerOfBlittableStructsKeepsItsStride)
{
    auto* Structs = (FVecStructsFn)DotNet::ResolveManagedExport("Test_VectorBindingStructs");
    ASSERT_NE(Structs, nullptr);

    float LastY = 0.0f;
    EXPECT_EQ(Structs(6, &LastY), 6);
    EXPECT_FLOAT_EQ(LastY, 10.0f);
}

// A script callback crosses as the handle owning its closure, so firing it has to run that closure once and
// leave the handle freed rather than dangling.
TEST_F(FFrameMarshalTest, AScriptCallbackRunsOnceAndReleasesItsHandle)
{
    auto* Bind = (FBindCallbackFn)DotNet::ResolveManagedExport("Test_BindScriptCallback");
    auto* Result = (FCallbackResFn)DotNet::ResolveManagedExport("Test_ScriptCallbackResult");
    ASSERT_NE(Bind, nullptr);
    ASSERT_NE(Result, nullptr);

    FScriptCallback Callback;
    Callback.Token = Bind();
    ASSERT_TRUE(Callback.IsBound());

    int32 Runs = -1;
    EXPECT_EQ(Result(&Runs), 0xFFFFFFFFULL);
    EXPECT_EQ(Runs, 0) << "binding a callback must not run it";

    Scripting::InvokeScriptCallback(Callback, 4242);
    EXPECT_EQ(Result(&Runs), 4242ULL);
    EXPECT_EQ(Runs, 1);

    // The handle is freed by the first run, so a second call finds nothing and must not run the closure again.
    Scripting::InvokeScriptCallback(Callback, 99);
    EXPECT_EQ(Result(&Runs), 4242ULL);
    EXPECT_EQ(Runs, 1) << "a one-shot callback ran twice";
}

// A repeating callback is what a looping timer and a tween value step need, so firing it must leave the
// handle alive and only the explicit release may free it.
TEST_F(FFrameMarshalTest, ARepeatingScriptCallbackSurvivesEveryFireUntilReleased)
{
    auto* Bind = (FBindCallbackFn)DotNet::ResolveManagedExport("Test_BindRepeatingScriptCallback");
    auto* Result = (FCallbackResFn)DotNet::ResolveManagedExport("Test_ScriptCallbackResult");
    ASSERT_NE(Bind, nullptr);
    ASSERT_NE(Result, nullptr);

    FScriptCallback Callback;
    Callback.Token = Bind();
    ASSERT_TRUE(Callback.IsBound());

    int32 Runs = -1;
    for (int32 i = 1; i <= 3; ++i)
    {
        Scripting::InvokeScriptCallbackRepeating(Callback, Scripting::PackFloatPayload(0.25f * (float)i));
        EXPECT_EQ(Result(&Runs), (uint64)(int64)(250 * i)) << "float payload did not survive the crossing";
        EXPECT_EQ(Runs, i) << "a repeating callback stopped firing";
    }

    // Releasing is the only thing that frees it, so a fire afterwards must find nothing to run.
    Scripting::ReleaseScriptCallback(Callback);
    Scripting::InvokeScriptCallbackRepeating(Callback, Scripting::PackFloatPayload(9.0f));
    EXPECT_EQ(Runs, 3) << "a released callback ran again";
}

// The owner is what a tween or timer captures, so dropping it has to be the release.
TEST_F(FFrameMarshalTest, AScriptCallbackOwnerReleasesWhenTheThingHoldingItDies)
{
    auto* Bind = (FBindCallbackFn)DotNet::ResolveManagedExport("Test_BindRepeatingScriptCallback");
    auto* Result = (FCallbackResFn)DotNet::ResolveManagedExport("Test_ScriptCallbackResult");
    ASSERT_NE(Bind, nullptr);
    ASSERT_NE(Result, nullptr);

    FScriptCallback Callback;
    Callback.Token = Bind();
    ASSERT_TRUE(Callback.IsBound());

    int32 Runs = -1;
    {
        const FScriptCallbackOwner Owner(Callback);
        Owner.Invoke(Scripting::PackFloatPayload(1.0f));
        EXPECT_EQ(Result(&Runs), 1000ULL);
        EXPECT_EQ(Runs, 1);
    }

    Scripting::InvokeScriptCallbackRepeating(Callback, Scripting::PackFloatPayload(2.0f));
    EXPECT_EQ(Runs, 1) << "the owner going out of scope did not release the handle";
}

TEST_F(FFrameMarshalTest, AnUnboundScriptCallbackIsSafeToInvoke)
{
    FScriptCallback Unbound;
    EXPECT_FALSE(Unbound.IsBound());
    Scripting::InvokeScriptCallback(Unbound, 7);
}

// The managed wrapper revalidates a CObject by reading its array entry rather than crossing back, so the
// case that matters is that a dead object still fails loudly instead of handing back reclaimed memory.
TEST_F(FFrameMarshalTest, AWrapperToADestroyedObjectStillThrows)
{
    auto* Bind = (FBindWrapperFn)DotNet::ResolveManagedExport("Test_BindObjectWrapper");
    auto* Probe = (FProbeWrapperFn)DotNet::ResolveManagedExport("Test_ProbeObjectWrapper");
    auto* Free = (FFreeWrapperFn)DotNet::ResolveManagedExport("Test_FreeObjectWrapper");
    ASSERT_NE(Bind, nullptr);
    ASSERT_NE(Probe, nullptr);
    ASSERT_NE(Free, nullptr);

    CObject* Doomed = NewObject(CEntityScriptTest::StaticClass(), nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Doomed, nullptr);

    // A strong reference, so the release below is the one that actually frees it.
    TObjectPtr<CObject> Owner(Doomed);
    void* Wrapper = Bind(Doomed);
    ASSERT_NE(Wrapper, nullptr);

    int32 bValid = 0;
    EXPECT_EQ(Probe(Wrapper, &bValid), 1) << "a live object must read through the fast path";
    EXPECT_EQ(bValid, 1);

    FScopedStaleReferenceTolerance Tolerance;
    GObjectArray.ReleaseStrongRef(Doomed);
    ASSERT_EQ(Owner.Get(), nullptr) << "the object under test was not actually freed";

    EXPECT_EQ(Probe(Wrapper, &bValid), -1) << "a destroyed object must throw, not hand back freed memory";
    EXPECT_EQ(bValid, 0) << "IsValid must agree with the handle read";

    Free(Wrapper);
}

// The generated invoker is an optimization over a path that still works without it, so the only way to know
// it is carrying the call is to ask whether one exists for a signature it should cover and one it should not.
TEST_F(FFrameMarshalTest, AGeneratedInvokerCoversABlittableSignatureAndDeclinesTheRest)
{
    auto* Has = (FHasInvokerFn)DotNet::ResolveManagedExport("Test_HasGeneratedInvoker");
    ASSERT_NE(Has, nullptr);

    EXPECT_EQ(Has(0), 1) << "an (int, out int) signature is exactly what the generator is for";
    EXPECT_EQ(Has(1), 1) << "a ref parameter writes back through the same generated path";
    EXPECT_EQ(Has(2), 0) << "a container view is not a blittable slot, so reflection has to keep it";
}

// Dispatch answers a repeat call off a direct-mapped line rather than hashing, and a line holds one entry.
// Alternating two functions makes every call miss the line the previous one just filled, which is the case
// that would silently run the wrong method if the line's identity check were ever wrong.
TEST_F(FFrameMarshalTest, AlternatingTwoFunctionsKeepsEachBoundToItsOwnMethod)
{
    auto* MakeTarget = (FMakeTargetFn)DotNet::ResolveManagedExport("Test_MakeMarshalTarget");
    auto* FreeTarget = (FFreeTargetFn)DotNet::ResolveManagedExport("Test_FreeMarshalTarget");
    auto* Invoke     = (FInvokeFn)DotNet::ResolveManagedExport("InvokeScriptFunction");
    ASSERT_NE(MakeTarget, nullptr);
    ASSERT_NE(Invoke, nullptr);

    Scripting::FScriptExportSchema OutParams;
    OutParams.Fields.push_back(ParamField("In", EPropertyTypeFlags::Int32, EPropertyFlags::None));
    OutParams.Fields.push_back(ParamField("Out", EPropertyTypeFlags::Int32, EPropertyFlags::OutParam));
    FFunction* OutFunction = MintFrame("FrameMarshal_AltOut", OutParams, "MarshalOut");
    ASSERT_NE(OutFunction, nullptr);

    Scripting::FScriptExportSchema RefParams;
    RefParams.Fields.push_back(ParamField("Value", EPropertyTypeFlags::Int32,
        EPropertyFlags::OutParam | EPropertyFlags::RefParam));
    FFunction* RefFunction = MintFrame("FrameMarshal_AltRef", RefParams, "MarshalRef");
    ASSERT_NE(RefFunction, nullptr);

    void* Target = MakeTarget();
    ASSERT_NE(Target, nullptr);

    for (int32 Round = 0; Round < 4; ++Round)
    {
        FFunctionFrame OutFrame(*OutFunction);
        OutFrame.At<int32>(0) = Round;
        OutFrame.At<int32>(1) = 0;
        Invoke(Target, OutFunction, OutFrame.GetMemory());
        EXPECT_EQ(OutFrame.At<int32>(1), Round * 2) << "MarshalOut was not the method that ran on round " << Round;

        FFunctionFrame RefFrame(*RefFunction);
        RefFrame.At<int32>(0) = Round;
        Invoke(Target, RefFunction, RefFrame.GetMemory());
        EXPECT_EQ(RefFrame.At<int32>(0), Round + 5) << "MarshalRef was not the method that ran on round " << Round;
    }

    FreeTarget(Target);
}

// Once the binder has published, native dispatch stops routing through the managed dispatcher and calls the
// generated entry point itself, so what matters is that publishing happens and the direct call agrees.
TEST_F(FFrameMarshalTest, ADispatchedFunctionPublishesAnInvokerNativeCanCallDirectly)
{
    auto* MakeTarget = (FMakeTargetFn)DotNet::ResolveManagedExport("Test_MakeMarshalTarget");
    auto* FreeTarget = (FFreeTargetFn)DotNet::ResolveManagedExport("Test_FreeMarshalTarget");
    auto* Invoke     = (FInvokeFn)DotNet::ResolveManagedExport("InvokeScriptFunction");
    ASSERT_NE(MakeTarget, nullptr);
    ASSERT_NE(Invoke, nullptr);

    Scripting::FScriptExportSchema Params;
    Params.Fields.push_back(ParamField("In", EPropertyTypeFlags::Int32, EPropertyFlags::None));
    Params.Fields.push_back(ParamField("Out", EPropertyTypeFlags::Int32, EPropertyFlags::OutParam));

    FFunction* Function = MintFrame("FrameMarshal_Published", Params, "MarshalOut");
    ASSERT_NE(Function, nullptr);
    EXPECT_EQ(Function->GetManagedInvoker(), nullptr) << "nothing should be published before the first call";

    void* Target = MakeTarget();
    ASSERT_NE(Target, nullptr);

    {
        FFunctionFrame Frame(*Function);
        Frame.At<int32>(0) = 21;
        Invoke(Target, Function, Frame.GetMemory());
        EXPECT_EQ(Frame.At<int32>(1), 42);
    }

    void* Published = Function->GetManagedInvoker();
    ASSERT_NE(Published, nullptr) << "the first dispatch should have published the generated entry point";
    ASSERT_NE(Function->GetManagedOffsets(), nullptr);

    // The same call the native thunk now makes, which has to produce the same answer as going through managed.
    using FDirect = void (*)(void*, void*, const int32*);
    FFunctionFrame Direct(*Function);
    Direct.At<int32>(0) = 50;
    reinterpret_cast<FDirect>(Published)(Target, Direct.GetMemory(), Function->GetManagedOffsets());
    EXPECT_EQ(Direct.At<int32>(1), 100) << "the direct call disagreed with the dispatcher";

    FreeTarget(Target);
}
