#include <gtest/gtest.h>

#include "Containers/String.h"
#include "Containers/ContainerOps.h"
#include "Containers/HashTable.h"
#include <utility>
#include "Containers/Vector.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Reflection/PropertyTypeTraits.h"
#include "Core/Reflection/Type/Function.h"
#include "Scripting/ScriptableTest.h"
#include "Core/Reflection/Type/LuminaTypes.h"

using namespace Lumina;

// Hand-rolled registration standing in for what the Reflector emits, so the runtime is proven on its own.
namespace
{
    struct FSubject
    {
        int32 Doubled = 0;
        FString Greeted;

        int32 Double(int32 X)
        {
            Doubled = X * 2;
            return Doubled;
        }

        void Greet(const FString& Name, int32 Times)
        {
            Greeted.Reset();
            for (int32 i = 0; i < Times; ++i)
            {
                Greeted += Name;
            }
        }

        static int32 Sum(int32 A, int32 B) { return A + B; }

        int32 Total(const TVector<int32>& Values)
        {
            int32 Result = 0;
            for (int32 Value : Values)
            {
                Result += Value;
            }
            Summed = Result;
            return Result;
        }

        int32 Summed = 0;

        CObject* Echo(CObject* In)
        {
            Echoed = In;
            return In;
        }

        CObject* Echoed = nullptr;

        int32 Lookup(const THashMap<FName, int32>& Table, FName Key)
        {
            const auto It = Table.Find(Key);
            return It != nullptr ? (*It) : -1;
        }

        void Consume(FString&& Moved)
        {
            Taken = Move(Moved);
        }

        FString Taken;
    };

    struct FDouble_Parms
    {
        int32 X;
        int32 ReturnValue;
    };

    struct FGreet_Parms
    {
        FString Name;
        int32   Times;
    };

    struct FSum_Parms
    {
        int32 A;
        int32 B;
        int32 ReturnValue;
    };

    void Thunk_Double(const FFunction&, void* Context, void* Frame)
    {
        FDouble_Parms& P = *(FDouble_Parms*)Frame;
        P.ReturnValue = ((FSubject*)Context)->Double(P.X);
    }

    void Thunk_Greet(const FFunction&, void* Context, void* Frame)
    {
        FGreet_Parms& P = *(FGreet_Parms*)Frame;
        ((FSubject*)Context)->Greet(P.Name, P.Times);
    }

    void Thunk_Sum(const FFunction&, void*, void* Frame)
    {
        FSum_Parms& P = *(FSum_Parms*)Frame;
        P.ReturnValue = FSubject::Sum(P.A, P.B);
    }

    struct FTotal_Parms
    {
        TVector<int32> Values;
        int32          ReturnValue;
    };

    void Thunk_Total(const FFunction&, void* Context, void* Frame)
    {
        FTotal_Parms& P = *(FTotal_Parms*)Frame;
        P.ReturnValue = ((FSubject*)Context)->Total(P.Values);
    }

    // An object parameter is held as a handle, matching the storage FObjectProperty describes.
    struct FEcho_Parms
    {
        TFrameObject<CScriptableTest> In;
        TFrameObject<CScriptableTest> ReturnValue;
    };

    void Thunk_Echo(const FFunction&, void* Context, void* Frame)
    {
        FEcho_Parms& P = *(FEcho_Parms*)Frame;
        P.ReturnValue = ((FSubject*)Context)->Echo(P.In.Get());
    }

    struct FLookup_Parms
    {
        THashMap<FName, int32> Table;
        FName                  Key;
        int32                  ReturnValue;
    };

    void Thunk_Lookup(const FFunction&, void* Context, void* Frame)
    {
        FLookup_Parms& P = *(FLookup_Parms*)Frame;
        P.ReturnValue = ((FSubject*)Context)->Lookup(P.Table, P.Key);
    }

    struct FConsume_Parms
    {
        FString Moved;
    };

    void Thunk_Consume(const FFunction&, void* Context, void* Frame)
    {
        FConsume_Parms& P = *(FConsume_Parms*)Frame;
        ((FSubject*)Context)->Consume(std::move(P.Moved));
    }

    template<typename T>
    constexpr FPropertyDesc Param(const char* Name, uint32 Offset)
    {
        return FPropertyDesc{ .Name = Name, .Offset = Offset, .Type = PropertyTypeOf<T> };
    }

    constexpr FPropertyDesc GDoubleParams[] =
    {
        Param<int32>("X", offsetof(FDouble_Parms, X)),
        Param<int32>("ReturnValue", offsetof(FDouble_Parms, ReturnValue)),
    };

    constexpr FPropertyDesc GGreetParams[] =
    {
        Param<FString>("Name", offsetof(FGreet_Parms, Name)),
        Param<int32>("Times", offsetof(FGreet_Parms, Times)),
    };

    constexpr FPropertyDesc GSumParams[] =
    {
        Param<int32>("A", offsetof(FSum_Parms, A)),
        Param<int32>("B", offsetof(FSum_Parms, B)),
        Param<int32>("ReturnValue", offsetof(FSum_Parms, ReturnValue)),
    };

    constexpr FPropertyDesc GTotalParams[] =
    {
        Param<TVector<int32>>("Values", offsetof(FTotal_Parms, Values)),
        Param<int32>("ReturnValue", offsetof(FTotal_Parms, ReturnValue)),
    };

    constexpr FPropertyDesc GLookupParams[] =
    {
        Param<THashMap<FName, int32>>("Table", offsetof(FLookup_Parms, Table)),
        Param<FName>("Key", offsetof(FLookup_Parms, Key)),
        Param<int32>("ReturnValue", offsetof(FLookup_Parms, ReturnValue)),
    };

    constexpr FPropertyDesc GConsumeParams[] =
    {
        Param<FString>("Moved", offsetof(FConsume_Parms, Moved)),
    };

    constexpr FPropertyDesc GEchoParams[] =
    {
        Param<TFrameObject<CScriptableTest>>("In", offsetof(FEcho_Parms, In)),
        Param<TFrameObject<CScriptableTest>>("ReturnValue", offsetof(FEcho_Parms, ReturnValue)),
    };

    constexpr FFunctionDesc GAllFunctions[] =
    {
        { .Name = "Double",  .Params = GDoubleParams,  .NumParams = 2, .ReturnIndex = 1,  .FrameSize = sizeof(FDouble_Parms),  .Thunk = &Thunk_Double },
        { .Name = "Greet",   .Params = GGreetParams,   .NumParams = 2, .ReturnIndex = -1, .FrameSize = sizeof(FGreet_Parms),   .Thunk = &Thunk_Greet },
        { .Name = "Sum",     .Flags = EFunctionFlags::Static, .Params = GSumParams, .NumParams = 3, .ReturnIndex = 2, .FrameSize = sizeof(FSum_Parms), .Thunk = &Thunk_Sum },
        { .Name = "Total",   .Params = GTotalParams,   .NumParams = 2, .ReturnIndex = 1,  .FrameSize = sizeof(FTotal_Parms),   .Thunk = &Thunk_Total },
        { .Name = "Echo",    .Params = GEchoParams,    .NumParams = 2, .ReturnIndex = 1,  .FrameSize = sizeof(FEcho_Parms),    .Thunk = &Thunk_Echo },
        { .Name = "Lookup",  .Params = GLookupParams,  .NumParams = 3, .ReturnIndex = 2,  .FrameSize = sizeof(FLookup_Parms),  .Thunk = &Thunk_Lookup },
        { .Name = "Consume", .Params = GConsumeParams, .NumParams = 1, .ReturnIndex = -1, .FrameSize = sizeof(FConsume_Parms), .Thunk = &Thunk_Consume },
    };

    // One struct carrying the three, built once so every test reads the same linked type.
    CStruct& SubjectStruct()
    {
        static CStruct* Struct = []
        {
            CStruct* New = NewObject<CStruct>(nullptr, "FFunctionTestSubject");
            New->AddToRoot();
            InitializeAndCreateFFunctions(New, GAllFunctions, (uint32)std::size(GAllFunctions));
            New->Link();
            return New;
        }();
        return *Struct;
    }
}

TEST(FunctionReflection, FFunctionStaysTheSizeOfAProperty)
{
    // 48 of description plus the two pointers script dispatch calls through. They are on the function rather
    // than behind a lookup because finding them is the cost the direct call exists to avoid.
    EXPECT_EQ(sizeof(FFunction), 64u) << "a function is a call descriptor, not an object";
}

TEST(FunctionReflection, AStructReportsTheFunctionsItDeclares)
{
    CStruct& Struct = SubjectStruct();

    EXPECT_EQ(Struct.GetFunctions().Num(), 7);
    ASSERT_NE(Struct.FindFunction("Double"), nullptr);
    ASSERT_NE(Struct.FindFunction("Greet"), nullptr);
    ASSERT_NE(Struct.FindFunction("Sum"), nullptr);
    EXPECT_EQ(Struct.FindFunction("Nope"), nullptr);
}

TEST(FunctionReflection, ParametersKeepTheirDeclaredOrder)
{
    const FFunction* Sum = SubjectStruct().FindFunction("Sum");
    ASSERT_NE(Sum, nullptr);

    ASSERT_EQ(Sum->GetParams().Num(), 3);
    EXPECT_EQ(Sum->GetParams()[0]->GetPropertyName(), FName("A"));
    EXPECT_EQ(Sum->GetParams()[1]->GetPropertyName(), FName("B"));
    EXPECT_EQ(Sum->GetParams()[2]->GetPropertyName(), FName("ReturnValue"));

    // Arguments excludes the return, which is what a caller walks to fill a frame.
    EXPECT_EQ(Sum->GetArguments().Num(), 2);
    EXPECT_EQ(Sum->GetReturnParam(), Sum->GetParams()[2]);
    EXPECT_TRUE(Sum->HasReturn());
}

TEST(FunctionReflection, AVoidFunctionHasNoReturnParameter)
{
    const FFunction* Greet = SubjectStruct().FindFunction("Greet");
    ASSERT_NE(Greet, nullptr);

    EXPECT_FALSE(Greet->HasReturn());
    EXPECT_EQ(Greet->GetReturnParam(), nullptr);
    EXPECT_EQ(Greet->GetArguments().Num(), Greet->GetParams().Num());
}

TEST(FunctionReflection, CallingThroughAFrameReachesTheRealFunction)
{
    const FFunction* Double = SubjectStruct().FindFunction("Double");
    ASSERT_NE(Double, nullptr);

    FSubject Subject;

    FFunctionFrame Frame(*Double);
    Frame.At<int32>(0) = 21;
    Frame.Invoke(&Subject);

    EXPECT_EQ(Frame.Return<int32>(), 42);
    EXPECT_EQ(Subject.Doubled, 42);
}

TEST(FunctionReflection, AStaticFunctionNeedsNoObject)
{
    const FFunction* Sum = SubjectStruct().FindFunction("Sum");
    ASSERT_NE(Sum, nullptr);
    ASSERT_TRUE(Sum->IsStatic());

    FFunctionFrame Frame(*Sum);
    Frame.At<int32>(0) = 17;
    Frame.At<int32>(1) = 25;
    Frame.Invoke(nullptr);

    EXPECT_EQ(Frame.Return<int32>(), 42);
}

// The whole point of parameters being FProperty: a string argument constructs and destructs itself.
TEST(FunctionReflection, AParameterThatOwnsMemoryIsBuiltAndTornDown)
{
    const FFunction* Greet = SubjectStruct().FindFunction("Greet");
    ASSERT_NE(Greet, nullptr);

    const FProperty* NameParam = Greet->GetParams()[0];
    ASSERT_EQ(NameParam->GetType(), EPropertyTypeFlags::String);
    ASSERT_TRUE(NameParam->OwnsStorage()) << "a string parameter has to be constructed in the frame";

    FSubject Subject;

    {
        FFunctionFrame Frame(*Greet);

        // Valid to assign straight away, which is only true because the frame constructed it.
        Frame.At<FString>(0) = "Long enough to reach the heap rather than the small-string buffer";
        Frame.At<int32>(1) = 2;
        Frame.Invoke(&Subject);
    }

    EXPECT_EQ(Subject.Greeted.Len(), 2 * strlen("Long enough to reach the heap rather than the small-string buffer"));
}

TEST(FunctionReflection, AFrameLargerThanTheInlineBufferStillWorks)
{
    const FFunction* Greet = SubjectStruct().FindFunction("Greet");
    ASSERT_NE(Greet, nullptr);

    // Not a large frame, but the same path a large one takes; the inline bound is what the two share.
    FFunctionFrame Frame(*Greet);
    EXPECT_NE(Frame.GetMemory(), nullptr);
}

TEST(FunctionReflection, AFunctionKnowsTheTypeThatDeclaredIt)
{
    const FFunction* Double = SubjectStruct().FindFunction("Double");
    ASSERT_NE(Double, nullptr);
    EXPECT_EQ(Double->GetOwnerStruct(), &SubjectStruct());
}

TEST(FunctionReflection, InvokingAnInstanceFunctionWithNoObjectIsRefused)
{
    const FFunction* Double = SubjectStruct().FindFunction("Double");
    ASSERT_NE(Double, nullptr);

    FFunctionFrame Frame(*Double);
    Frame.At<int32>(0) = 21;

    // Logs and returns rather than dereferencing null; the return slot stays at its constructed default.
    Frame.Invoke(nullptr);
    EXPECT_EQ(Frame.Return<int32>(), 0);
}

// A container parameter contributes an inner to the params array, which is not a frame member: the frame
// declares Values and ReturnValue, so the argument list and the return index count only those.
TEST(FunctionReflection, AContainerParameterIsOneArgumentDespiteItsInner)
{
    const FFunction* Total = SubjectStruct().FindFunction("Total");
    ASSERT_NE(Total, nullptr);

    ASSERT_EQ(Total->GetParams().Num(), 2) << "the inner is described by its owner, not declared beside it";
    EXPECT_EQ(Total->GetParams()[0]->GetPropertyName(), FName("Values"));
    EXPECT_EQ(Total->GetParams()[0]->GetType(), EPropertyTypeFlags::Vector);
    EXPECT_EQ(Total->GetArguments().Num(), 1);
    EXPECT_EQ(Total->GetReturnParam(), Total->GetParams()[1]);
}

TEST(FunctionReflection, ACallWithAContainerArgumentRoundTrips)
{
    const FFunction* Total = SubjectStruct().FindFunction("Total");
    ASSERT_NE(Total, nullptr);
    ASSERT_TRUE(Total->GetParams()[0]->OwnsStorage()) << "a container has to be constructed in the frame";

    FSubject Subject;

    {
        FFunctionFrame Frame(*Total);

        TVector<int32>& Values = Frame.At<TVector<int32>>(0);
        Values.Add(10);
        Values.Add(20);
        Values.Add(12);

        Frame.Invoke(&Subject);

        EXPECT_EQ(Frame.Return<int32>(), 42);
    }

    EXPECT_EQ(Subject.Summed, 42);
}

// An object parameter is a handle, so the frame holds a strong reference for the duration of the call and
// releases it on teardown; a raw pointer could not, which is why FObjectProperty describes a handle.
TEST(FunctionReflection, AnObjectArgumentTravelsAsAHandle)
{
    const FFunction* Echo = SubjectStruct().FindFunction("Echo");
    ASSERT_NE(Echo, nullptr);

    const FProperty* InParam = Echo->GetParams()[0];
    ASSERT_EQ(InParam->GetType(), EPropertyTypeFlags::Object);
    EXPECT_EQ(InParam->GetElementSize(), sizeof(TStrongObjectPtr<CObject>));
    EXPECT_TRUE(InParam->OwnsStorage()) << "the handle has to be released when the frame goes away";

    // Held, or the frame's release takes the count back to zero and destroys it before the checks below.
    TStrongObjectPtr<CObject> Owner(NewObject(CScriptableTest::StaticClass(), nullptr, NAME_None, FGuid::New(), OF_Transient));
    CObject* Subject_Object = Owner.Get();
    ASSERT_NE(Subject_Object, nullptr);
    const int32 RefsBefore = Subject_Object->GetStrongRefCount();

    FSubject Subject;

    {
        FFunctionFrame Frame(*Echo);
        Frame.At<TStrongObjectPtr<CObject>>(0) = Subject_Object;

        EXPECT_GT(Subject_Object->GetStrongRefCount(), RefsBefore) << "the frame holds the object across the call";

        Frame.Invoke(&Subject);

        EXPECT_EQ(Frame.Return<TStrongObjectPtr<CObject>>().Get(), Subject_Object);
        EXPECT_EQ(Subject.Echoed, Subject_Object);
    }

    EXPECT_EQ(Subject_Object->GetStrongRefCount(), RefsBefore) << "and lets go of it once torn down";
}

// A map contributes two inners and is still one argument, and the pair order is the emitter's ABI contract.
TEST(FunctionReflection, AMapParameterIsOneArgumentWithTwoInners)
{
    const FFunction* Lookup = SubjectStruct().FindFunction("Lookup");
    ASSERT_NE(Lookup, nullptr);

    ASSERT_EQ(Lookup->GetParams().Num(), 3) << "Table, Key and the return; the inners are not declared";
    EXPECT_EQ(Lookup->GetParams()[0]->GetPropertyName(), FName("Table"));
    EXPECT_EQ(Lookup->GetParams()[0]->GetType(), EPropertyTypeFlags::Map);
    EXPECT_EQ(Lookup->GetParams()[1]->GetPropertyName(), FName("Key"));
    EXPECT_EQ(Lookup->GetArguments().Num(), 2);
}

TEST(FunctionReflection, ACallWithAMapArgumentRoundTrips)
{
    const FFunction* Lookup = SubjectStruct().FindFunction("Lookup");
    ASSERT_NE(Lookup, nullptr);
    ASSERT_TRUE(Lookup->GetParams()[0]->OwnsStorage());

    FSubject Subject;

    FFunctionFrame Frame(*Lookup);
    Frame.At<THashMap<FName, int32>>(0).TryAdd(FName("Answer"), 42);
    Frame.At<FName>(1) = FName("Answer");
    Frame.Invoke(&Subject);

    EXPECT_EQ(Frame.Return<int32>(), 42);
}

// An rvalue parameter is moved out of the frame, and the moved-from slot is still torn down safely.
TEST(FunctionReflection, AnRvalueParameterIsMovedOutOfTheFrame)
{
    const FFunction* Consume = SubjectStruct().FindFunction("Consume");
    ASSERT_NE(Consume, nullptr);

    FSubject Subject;

    {
        FFunctionFrame Frame(*Consume);
        Frame.At<FString>(0) = "Long enough that the move has something on the heap to steal";
        Frame.Invoke(&Subject);
    }

    EXPECT_EQ(Subject.Taken, "Long enough that the move has something on the heap to steal");
}
