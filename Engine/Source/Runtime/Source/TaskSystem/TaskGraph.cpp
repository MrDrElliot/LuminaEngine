#include "RuntimePCH.h"
#include "TaskGraph.h"
#include "Task.h"
#include "Scheduler/JobScheduler.h"
#include "Core/Threading/Atomic.h"

#include <coroutine>

namespace Lumina
{
    struct FTaskGraph::FNode
    {
        void*                           Callable       = nullptr;
        FInvokeOneShot                  InvokeOneShot  = nullptr;
        FInvokeParallel                 InvokeParallel = nullptr;
        FDestroyCallable                Destroy        = nullptr;
        bool                            bIsParallelFor = false;
        uint32                          SetSize        = 1;
        uint32                          MinRange       = 1;
        ETaskPriority                   Priority       = ETaskPriority::Medium;

        FTaskGraph*                     Graph          = nullptr;
        FBlockLinearAllocator*          Arena          = nullptr;
        uint32                          Index          = 0;

        TAtomic<int32>                  PendingDeps{0};
        TFrameVector<uint32>            Dependents;

        std::coroutine_handle<>         CoroHandle{};

        ~FNode()
        {
            // Closure lives in the arena; run its destructor, never free.
            if (Callable && Destroy)
            {
                Destroy(Callable);
            }
        }
    };

    struct FTaskGraph::FNodeCoro
    {
        struct promise_type
        {
            // The coroutine's FNode* is forwarded here so the frame lands in the graph arena.
            static void* operator new(std::size_t Size, FNode* Node)
            {
                return Node->Arena->Allocate(Size, alignof(std::max_align_t));
            }
            static void operator delete(void* /*Ptr*/) noexcept {}
            static void operator delete(void* /*Ptr*/, std::size_t) noexcept {}

            FNodeCoro get_return_object() noexcept
            {
                return FNodeCoro{ std::coroutine_handle<promise_type>::from_promise(*this) };
            }

            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend()   noexcept { return {}; }
            void                return_void()     noexcept {}
            void                unhandled_exception() { ASSERT(false); }
        };

        std::coroutine_handle<promise_type> Handle;
    };

    FTaskGraph::FNodeCoro FTaskGraph::RunNode(FNode* Node)
    {
        if (Node->bIsParallelFor)
        {
            if (Node->InvokeParallel)
            {
                FNode* N = Node;
                co_await Task::ParallelForAsync(N->SetSize, N->MinRange,
                    [N](const Task::FParallelRange& R) { N->InvokeParallel(N->Callable, R); },
                    N->Priority);
            }
        }
        else if (Node->InvokeOneShot)
        {
            Node->InvokeOneShot(Node->Callable);
        }

        FTaskGraph::CompleteNode(Node);
        co_return;
    }

    void FTaskGraph::StartNode(FNode* Node)
    {
        CoroDetail::ScheduleResume(Node->CoroHandle, Node->Priority);
    }

    void FTaskGraph::CompleteNode(FNode* Node)
    {
        FTaskGraph* Graph = Node->Graph;

        for (uint32 DepIndex : Node->Dependents)
        {
            FNode* Dependent = Graph->Nodes[DepIndex];
            if (Dependent->PendingDeps.fetch_sub(1, std::memory_order_acq_rel) - 1 == 0)
            {
                StartNode(Dependent);
            }
        }

        Jobs::DecrementCounter(Graph->GraphCounter, 1);
    }

    FTaskGraph::FTaskGraph()
        : Allocator(16llu * 1024)
    {}

    FTaskGraph::~FTaskGraph()
    {
        if (bDispatched)
        {
            Wait();
        }
        for (FNode* Node : Nodes)
        {
            Node->~FNode();
        }
    }

    void FTaskGraph::Reset()
    {
        if (bDispatched)
        {
            Wait();
        }
        for (FNode* Node : Nodes)
        {
            Node->~FNode();
        }
        Nodes.clear();
        Edges.clear();
        Allocator.Reset();
        bDispatched = false;
    }

    FTaskGraph::FNodeHandle FTaskGraph::AddOneShotNode(void* Callable, FInvokeOneShot Invoke, FDestroyCallable Destroy, ETaskPriority Priority)
    {
        auto* Node              = Allocator.TAlloc<FNode>();
        Node->Dependents.set_allocator(FFrameArenaAllocator(&Allocator, "TaskGraphDeps"));
        Node->Callable          = Callable;
        Node->InvokeOneShot     = Invoke;
        Node->Destroy           = Destroy;
        Node->bIsParallelFor    = false;
        Node->SetSize           = 1;
        Node->MinRange          = 1;
        Node->Priority          = Priority;
        Node->Arena             = &Allocator;

        FNodeHandle Handle{ static_cast<uint32>(Nodes.size()) };
        Nodes.push_back(Node);
        return Handle;
    }

    FTaskGraph::FNodeHandle FTaskGraph::AddParallelForNode(uint32 Count, uint32 MinRange, void* Callable, FInvokeParallel Invoke, FDestroyCallable Destroy, ETaskPriority Priority)
    {
        auto* Node              = Allocator.TAlloc<FNode>();
        Node->Dependents.set_allocator(FFrameArenaAllocator(&Allocator, "TaskGraphDeps"));
        Node->bIsParallelFor    = true;
        Node->Priority          = Priority;
        Node->Arena             = &Allocator;

        if (Count == 0)
        {
            // Empty work; node still fires dependents. No callable placed.
            Node->SetSize       = 0;
            Node->MinRange      = 1;
        }
        else
        {
            Node->Callable          = Callable;
            Node->InvokeParallel    = Invoke;
            Node->Destroy           = Destroy;
            Node->SetSize           = Count;
            Node->MinRange          = std::max(1u, MinRange);
        }

        FNodeHandle Handle{ static_cast<uint32>(Nodes.size()) };
        Nodes.push_back(Node);
        return Handle;
    }

    void FTaskGraph::AddDependency(FNodeHandle Node, FNodeHandle Dependency)
    {
        ASSERT(!bDispatched);
        ASSERT(Node.IsValid() && Dependency.IsValid());
        ASSERT(Node.Index < Nodes.size() && Dependency.Index < Nodes.size());
        Edges.emplace_back(Node.Index, Dependency.Index);
    }

    void FTaskGraph::Dispatch()
    {
        ASSERT(!bDispatched);
        ASSERT(GTaskSystem != nullptr);
        bDispatched = true;

        const uint32 NumNodes = static_cast<uint32>(Nodes.size());
        if (NumNodes == 0)
        {
            return;
        }

        for (uint32 i = 0; i < NumNodes; ++i)
        {
            FNode* Node = Nodes[i];
            Node->Index = i;
            Node->Graph = this;
            Node->PendingDeps.store(0, std::memory_order_relaxed);
            Node->Dependents.clear();
        }
        for (const auto& Edge : Edges)
        {
            // Edge = (child, parent): child depends on parent.
            Nodes[Edge.first]->PendingDeps.fetch_add(1, std::memory_order_relaxed);
            Nodes[Edge.second]->Dependents.push_back(Edge.first);
        }

        // Create node coroutines single-threaded; the arena is not thread-safe.
        for (uint32 i = 0; i < NumNodes; ++i)
        {
            Nodes[i]->CoroHandle = RunNode(Nodes[i]).Handle;
        }

        // Capture roots before scheduling, so a worker driving a dependent to zero can't race the loop.
        DispatchRoots.clear();
        for (uint32 i = 0; i < NumNodes; ++i)
        {
            if (Nodes[i]->PendingDeps.load(std::memory_order_relaxed) == 0)
            {
                DispatchRoots.push_back(i);
            }
        }

        GraphCounter = Jobs::AllocCounter(static_cast<int32>(NumNodes));

        for (uint32 RootIndex : DispatchRoots)
        {
            StartNode(Nodes[RootIndex]);
        }
    }

    void FTaskGraph::Wait()
    {
        if (!bDispatched)
        {
            return;
        }

        LUMINA_PROFILE_SECTION("FTaskGraph::Wait");

        if (GraphCounter != nullptr)
        {
            Jobs::WaitForCounter(GraphCounter, 0);
            Jobs::FreeCounter(GraphCounter);
            GraphCounter = nullptr;
        }

        bDispatched = false;
    }
}
