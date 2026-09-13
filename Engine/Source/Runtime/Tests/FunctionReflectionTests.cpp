#include <gtest/gtest.h>

#include "Containers/String.h"
#include "Containers/ContainerOps.h"
#include "Containers/Vector.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
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
            Greeted.clear();
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

    void Thunk_Double(void* Context, void* Frame)
    {
        FDouble_Parms& P = *(FDouble_Parms*)Frame;
        P.ReturnValue = ((FSubject*)Context)->Double(P.X);
    }

    void Thunk_Greet(void* Context, void* Frame)
    {
        FGreet_Parms& P = *(FGreet_Parms*)Frame;
        ((FSubject*)Context)->Greet(P.Name, P.Times);
    }

    void Thunk_Sum(void*, void* Frame)
    {
        FSum_Parms& P = *(FSum_Parms*)Frame;
        P.ReturnValue = FSubject::Sum(P.A, P.B);
    }

    struct FTotal_Parms
    {
        TVector<int32> Values;
        int32          ReturnValue;
    };

    void Thunk_Total(void* Context, void* Frame)
    {
        FTotal_Parms& P = *(FTotal_Parms*)Frame;
        P.ReturnValue = ((FSubject*)Context)->Total(P.Values);
    }

    const FVectorOps* TotalValuesOps() { return GetVectorOpsFor<TVector<int32>>(); }

    // An object parameter is held as a handle, matching the storage FObjectProperty describes.
    struct FEcho_Parms
    {
        TObjectPtr<CObject> In;
        TObjectPtr<CObject> ReturnValue;
    };

    void Thunk_Echo(void* Context, void* Frame)
    {
        FEcho_Parms& P = *(FEcho_Parms*)Frame;
        P.ReturnValue = (CObject*)(((FSubject*)Context)->Echo((CObject*)P.In.Get()));
    }

    const FNumericPropertyParams GDoubleX = { "X", EPropertyFlags::None, EPropertyTypeFlags::Int32, nullptr, nullptr, offsetof(FDouble_Parms, X) };
    const FNumericPropertyParams GDoubleRet = { "ReturnValue", EPropertyFlags::None, EPropertyTypeFlags::Int32, nullptr, nullptr, offsetof(FDouble_Parms, ReturnValue) };
    const FPropertyParams* const GDoubleParams[] = { (const FPropertyParams*)&GDoubleX, (const FPropertyParams*)&GDoubleRet };

    const FStringPropertyParams GGreetName = { "Name", EPropertyFlags::None, EPropertyTypeFlags::String, nullptr, nullptr, offsetof(FGreet_Parms, Name) };
    const FNumericPropertyParams GGreetTimes = { "Times", EPropertyFlags::None, EPropertyTypeFlags::Int32, nullptr, nullptr, offsetof(FGreet_Parms, Times) };
    const FPropertyParams* const GGreetParams[] = { (const FPropertyParams*)&GGreetName, (const FPropertyParams*)&GGreetTimes };

    const FNumericPropertyParams GSumA = { "A", EPropertyFlags::None, EPropertyTypeFlags::Int32, nullptr, nullptr, offsetof(FSum_Parms, A) };
    const FNumericPropertyParams GSumB = { "B", EPropertyFlags::None, EPropertyTypeFlags::Int32, nullptr, nullptr, offsetof(FSum_Parms, B) };
    const FNumericPropertyParams GSumRet = { "ReturnValue", EPropertyFlags::None, EPropertyTypeFlags::Int32, nullptr, nullptr, offsetof(FSum_Parms, ReturnValue) };
    const FPropertyParams* const GSumParams[] = { (const FPropertyParams*)&GSumA, (const FPropertyParams*)&GSumB, (const FPropertyParams*)&GSumRet };

    const FFunctionParams GDoubleFn = { "Double", EFunctionFlags::None, GDoubleParams, 2, 1, sizeof(FDouble_Parms), &Thunk_Double };
    const FFunctionParams GGreetFn  = { "Greet",  EFunctionFlags::None, GGreetParams,  2, -1, sizeof(FGreet_Parms),  &Thunk_Greet  };
    const FFunctionParams GSumFn    = { "Sum",    EFunctionFlags::Static, GSumParams,  3,  2, sizeof(FSum_Parms),    &Thunk_Sum    };

    // A container parameter, whose element is described by an inner sitting before it exactly as a member's is.
    const FNumericPropertyParams GTotalInner = { "Values_Inner", EPropertyFlags::SubField, EPropertyTypeFlags::Int32, nullptr, nullptr, 0 };
    const FArrayPropertyParams GTotalValues = { { "Values", EPropertyFlags::None, EPropertyTypeFlags::Vector, nullptr, nullptr, offsetof(FTotal_Parms, Values) }, &TotalValuesOps };
    const FNumericPropertyParams GTotalRet = { "ReturnValue", EPropertyFlags::None, EPropertyTypeFlags::Int32, nullptr, nullptr, offsetof(FTotal_Parms, ReturnValue) };
    const FPropertyParams* const GTotalParams[] = {
        (const FPropertyParams*)&GTotalInner,
        (const FPropertyParams*)&GTotalValues,
        (const FPropertyParams*)&GTotalRet,
    };

    const FFunctionParams GTotalFn = { "Total", EFunctionFlags::None, GTotalParams, 3, 1, sizeof(FTotal_Parms), &Thunk_Total };

    const FObjectPropertyParams GEchoIn = { { "In", EPropertyFlags::None, EPropertyTypeFlags::Object, nullptr, nullptr, offsetof(FEcho_Parms, In) }, &CScriptableTest::StaticClass };
    const FObjectPropertyParams GEchoRet = { { "ReturnValue", EPropertyFlags::None, EPropertyTypeFlags::Object, nullptr, nullptr, offsetof(FEcho_Parms, ReturnValue) }, &CScriptableTest::StaticClass };
    const FPropertyParams* const GEchoParams[] = {
        (const FPropertyParams*)&GEchoIn,
        (const FPropertyParams*)&GEchoRet,
    };

    const FFunctionParams GEchoFn = { "Echo", EFunctionFlags::None, GEchoParams, 2, 1, sizeof(FEcho_Parms), &Thunk_Echo };

    const FFunctionParams* const GAllFunctions[] = { &GDoubleFn, &GGreetFn, &GSumFn, &GTotalFn, &GEchoFn };

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
    EXPECT_EQ(sizeof(FFunction), 48u) << "a function is a call descriptor, not an object";
}

TEST(FunctionReflection, AStructReportsTheFunctionsItDeclares)
{
    CStruct& Struct = SubjectStruct();

    EXPECT_EQ(Struct.GetFunctions().size(), 5u);
    ASSERT_NE(Struct.FindFunction("Double"), nullptr);
    ASSERT_NE(Struct.FindFunction("Greet"), nullptr);
    ASSERT_NE(Struct.FindFunction("Sum"), nullptr);
    EXPECT_EQ(Struct.FindFunction("Nope"), nullptr);
}

TEST(FunctionReflection, ParametersKeepTheirDeclaredOrder)
{
    const FFunction* Sum = SubjectStruct().FindFunction("Sum");
    ASSERT_NE(Sum, nullptr);

    ASSERT_EQ(Sum->GetParams().size(), 3u);
    EXPECT_EQ(Sum->GetParams()[0]->GetPropertyName(), FName("A"));
    EXPECT_EQ(Sum->GetParams()[1]->GetPropertyName(), FName("B"));
    EXPECT_EQ(Sum->GetParams()[2]->GetPropertyName(), FName("ReturnValue"));

    // Arguments excludes the return, which is what a caller walks to fill a frame.
    EXPECT_EQ(Sum->GetArguments().size(), 2u);
    EXPECT_EQ(Sum->GetReturnParam(), Sum->GetParams()[2]);
    EXPECT_TRUE(Sum->HasReturn());
}

TEST(FunctionReflection, AVoidFunctionHasNoReturnParameter)
{
    const FFunction* Greet = SubjectStruct().FindFunction("Greet");
    ASSERT_NE(Greet, nullptr);

    EXPECT_FALSE(Greet->HasReturn());
    EXPECT_EQ(Greet->GetReturnParam(), nullptr);
    EXPECT_EQ(Greet->GetArguments().size(), Greet->GetParams().size());
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

    EXPECT_EQ(Subject.Greeted.size(), 2u * strlen("Long enough to reach the heap rather than the small-string buffer"));
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

    ASSERT_EQ(Total->GetParams().size(), 2u) << "the inner is described by its owner, not declared beside it";
    EXPECT_EQ(Total->GetParams()[0]->GetPropertyName(), FName("Values"));
    EXPECT_EQ(Total->GetParams()[0]->GetType(), EPropertyTypeFlags::Vector);
    EXPECT_EQ(Total->GetArguments().size(), 1u);
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
        Values.push_back(10);
        Values.push_back(20);
        Values.push_back(12);

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
    EXPECT_EQ(InParam->GetElementSize(), sizeof(TObjectPtr<CObject>));
    EXPECT_TRUE(InParam->OwnsStorage()) << "the handle has to be released when the frame goes away";

    CObject* Subject_Object = NewObject(CScriptableTest::StaticClass(), nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Subject_Object, nullptr);
    const int32 RefsBefore = Subject_Object->GetStrongRefCount();

    FSubject Subject;

    {
        FFunctionFrame Frame(*Echo);
        Frame.At<TObjectPtr<CObject>>(0) = Subject_Object;

        EXPECT_GT(Subject_Object->GetStrongRefCount(), RefsBefore) << "the frame holds the object across the call";

        Frame.Invoke(&Subject);

        EXPECT_EQ(Frame.Return<TObjectPtr<CObject>>().Get(), Subject_Object);
        EXPECT_EQ(Subject.Echoed, Subject_Object);
    }

    EXPECT_EQ(Subject_Object->GetStrongRefCount(), RefsBefore) << "and lets go of it once torn down";

    Subject_Object->ConditionalBeginDestroy();
}
