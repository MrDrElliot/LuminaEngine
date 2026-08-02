#include "RuntimePCH.h"
#include "ObjectInitializer.h"

#include "Containers/Array.h"
#include "Core/Threading/Thread.h"

namespace Lumina
{
    static TFixedVector<FObjectInitializer*, 4> InitializerStack;
    static FRecursiveMutex Mutex;
    
    FObjectInitializer::FObjectInitializer(CPackage* InPackage, const FConstructCObjectParams& InParams)
        : Package(InPackage)
        , Params(InParams)
    {
        Construct();
    }

    FObjectInitializer::~FObjectInitializer()
    {
        FRecursiveScopeLock Lock(Mutex);
        InitializerStack.pop_back();
    }

    FObjectInitializer* FObjectInitializer::Get()
    {
        return InitializerStack.empty() ? nullptr : InitializerStack.back();
    }

    void FObjectInitializer::Construct()
    {
        FRecursiveScopeLock Lock(Mutex);
        InitializerStack.push_back(this);
    }
}
