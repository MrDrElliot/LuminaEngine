#include <gtest/gtest.h>

#include "Events/EventProcessor.h"
#include "Events/Event.h"
#include "Containers/Function.h"

using namespace Lumina;

namespace
{
    class FRecordingHandler : public IEventHandler
    {
    public:
        explicit FRecordingHandler(TVector<FRecordingHandler*>* InCallLog, bool InConsume = false)
            : CallLog(InCallLog), bConsume(InConsume) {}

        virtual bool OnEvent(FEvent& Event) override
        {
            CallLog->Add(this);
            if (OnEventCallback)
            {
                OnEventCallback();
            }
            return bConsume;
        }

        TFunction<void()> OnEventCallback;

    private:
        TVector<FRecordingHandler*>* CallLog;
        bool bConsume;
    };
}

TEST(EventProcessor, LowerPriorityHandlerRunsAfterHigherPriority)
{
    FEventProcessor Processor;
    TVector<FRecordingHandler*> CallLog;

    FRecordingHandler Low(&CallLog);
    FRecordingHandler High(&CallLog);
    Processor.RegisterEventHandler(&Low, 0);
    Processor.RegisterEventHandler(&High, 100);

    Processor.Dispatch<FWindowFocusEvent>();

    ASSERT_EQ(CallLog.Num(), 2);
    EXPECT_EQ(CallLog[0], &High);
    EXPECT_EQ(CallLog[1], &Low);
}

TEST(EventProcessor, HandlerReturningTrueStopsPropagation)
{
    FEventProcessor Processor;
    TVector<FRecordingHandler*> CallLog;

    FRecordingHandler First(&CallLog, true);
    FRecordingHandler Second(&CallLog);
    Processor.RegisterEventHandler(&First, 100);
    Processor.RegisterEventHandler(&Second, 0);

    Processor.Dispatch<FWindowFocusEvent>();

    ASSERT_EQ(CallLog.Num(), 1);
    EXPECT_EQ(CallLog[0], &First);
}

TEST(EventProcessor, HandlerUnregisteringItselfMidDispatchDoesNotCrash)
{
    FEventProcessor Processor;
    TVector<FRecordingHandler*> CallLog;

    FRecordingHandler Self(&CallLog);
    FRecordingHandler Later(&CallLog);
    Processor.RegisterEventHandler(&Self, 100);
    Processor.RegisterEventHandler(&Later, 0);

    Self.OnEventCallback = [&]() { Processor.UnregisterEventHandler(&Self); };

    Processor.Dispatch<FWindowFocusEvent>();

    ASSERT_EQ(CallLog.Num(), 2);
    EXPECT_EQ(CallLog[0], &Self);
    EXPECT_EQ(CallLog[1], &Later);
}

TEST(EventProcessor, HandlerUnregisteringALaterHandlerMidDispatchSkipsIt)
{
    FEventProcessor Processor;
    TVector<FRecordingHandler*> CallLog;

    FRecordingHandler First(&CallLog);
    FRecordingHandler Later(&CallLog);
    Processor.RegisterEventHandler(&First, 100);
    Processor.RegisterEventHandler(&Later, 0);

    First.OnEventCallback = [&]() { Processor.UnregisterEventHandler(&Later); };

    Processor.Dispatch<FWindowFocusEvent>();

    ASSERT_EQ(CallLog.Num(), 1);
    EXPECT_EQ(CallLog[0], &First) << "the unregistered handler must not be invoked";
}

TEST(EventProcessor, SamePrioritySiblingStillRunsAfterSelfUnregister)
{
    FEventProcessor Processor;
    TVector<FRecordingHandler*> CallLog;

    FRecordingHandler First(&CallLog);
    FRecordingHandler Second(&CallLog);
    Processor.RegisterEventHandler(&First, 0);
    Processor.RegisterEventHandler(&Second, 0);

    First.OnEventCallback = [&]() { Processor.UnregisterEventHandler(&First); };

    Processor.Dispatch<FWindowFocusEvent>();

    ASSERT_EQ(CallLog.Num(), 2) << "a same-priority sibling must still run after the first unregisters itself";
    EXPECT_EQ(CallLog[0], &First);
    EXPECT_EQ(CallLog[1], &Second);
}

TEST(EventProcessor, RegisteringAHigherPriorityHandlerMidDispatchDoesNotDoubleCallAnyHandler)
{
    FEventProcessor Processor;
    TVector<FRecordingHandler*> CallLog;

    FRecordingHandler High(&CallLog);
    FRecordingHandler Low(&CallLog);
    FRecordingHandler Highest(&CallLog);
    Processor.RegisterEventHandler(&High, 100);
    Processor.RegisterEventHandler(&Low, 0);

    High.OnEventCallback = [&]() { Processor.RegisterEventHandler(&Highest, 200); };

    Processor.Dispatch<FWindowFocusEvent>();

    ASSERT_EQ(CallLog.Num(), 2) << "no handler may run twice";
    EXPECT_EQ(CallLog[0], &High);
    EXPECT_EQ(CallLog[1], &Low);
}

TEST(EventProcessor, HandlersUnregisteringDifferentLaterHandlersDoNotCrash)
{
    FEventProcessor Processor;
    TVector<FRecordingHandler*> CallLog;

    FRecordingHandler Handler1(&CallLog);
    FRecordingHandler Handler2(&CallLog);
    FRecordingHandler Handler3(&CallLog);
    FRecordingHandler Handler4(&CallLog);
    Processor.RegisterEventHandler(&Handler1, 300);
    Processor.RegisterEventHandler(&Handler2, 200);
    Processor.RegisterEventHandler(&Handler3, 100);
    Processor.RegisterEventHandler(&Handler4, 0);

    Handler1.OnEventCallback = [&]() { Processor.UnregisterEventHandler(&Handler3); };
    Handler2.OnEventCallback = [&]() { Processor.UnregisterEventHandler(&Handler4); };

    Processor.Dispatch<FWindowFocusEvent>();

    ASSERT_EQ(CallLog.Num(), 2);
    EXPECT_EQ(CallLog[0], &Handler1);
    EXPECT_EQ(CallLog[1], &Handler2);
}
