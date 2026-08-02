#include "SlowTask.h"

#include "Core/Threading/Thread.h"

namespace Lumina
{
    namespace
    {
        FMutex                      GSlowTaskMutex;
        TVector<FSlowTaskProgress>  GSlowTasks;
        uint64                      GNextSlowTaskID = 1;

        float ClampFraction(float Value)
        {
            if (Value < 0.0f) return 0.0f;
            if (Value > 1.0f) return 1.0f;
            return Value;
        }
    }

    FScopedSlowTask::FScopedSlowTask(float InTotalWork, FStringView InTitle, FStringView InDefaultMessage)
        : TotalWork(InTotalWork > 0.0f ? InTotalWork : 1.0f)
        , Title(InTitle.begin(), InTitle.end())
        , CurrentMessage(InDefaultMessage.begin(), InDefaultMessage.end())
    {
        FScopeLock Lock(GSlowTaskMutex);
        ID = GNextSlowTaskID++;

        FSlowTaskProgress& Entry = GSlowTasks.emplace_back();
        Entry.ID       = ID;
        Entry.Title    = Title;
        Entry.Message  = CurrentMessage;
        Entry.Fraction = 0.0f;
    }

    FScopedSlowTask::~FScopedSlowTask()
    {
        FScopeLock Lock(GSlowTaskMutex);
        for (auto It = GSlowTasks.begin(); It != GSlowTasks.end(); ++It)
        {
            if (It->ID == ID)
            {
                GSlowTasks.erase(It);
                break;
            }
        }
    }

    void FScopedSlowTask::EnterProgressFrame(float ExpectedWork, FStringView Message)
    {
        #if USING(WITH_EDITOR)
        FScopeLock Lock(GSlowTaskMutex);
        CompletedWork += ExpectedWork;
        if (!Message.empty())
        {
            CurrentMessage.assign(Message.begin(), Message.end());
        }
        Publish();
        #endif
    }

    void FScopedSlowTask::UpdateMessage(FStringView Message)
    {
        #if USING(WITH_EDITOR)
        FScopeLock Lock(GSlowTaskMutex);
        CurrentMessage.assign(Message.begin(), Message.end());
        Publish();
        #endif
    }

    void FScopedSlowTask::Publish()
    {
        for (FSlowTaskProgress& Entry : GSlowTasks)
        {
            if (Entry.ID == ID)
            {
                Entry.Message  = CurrentMessage;
                Entry.Fraction = ClampFraction(CompletedWork / TotalWork);
                return;
            }
        }
    }

    namespace SlowTasks
    {
        bool HasActive()
        {
            FScopeLock Lock(GSlowTaskMutex);
            return !GSlowTasks.empty();
        }

        TVector<FSlowTaskProgress> GetSnapshot()
        {
            FScopeLock Lock(GSlowTaskMutex);
            return GSlowTasks;
        }
    }
}
