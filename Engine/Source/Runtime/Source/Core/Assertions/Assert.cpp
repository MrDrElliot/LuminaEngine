#include "RuntimePCH.h"
#include <atomic>
#include <string>
#include "Assert.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"


namespace Lumina::Assert
{
    static constexpr FStringView AssertionTypeToString(EAssertionType Type)
    {
        switch (Type)
        {
            case EAssertionType::Assert:        return "ASSERTION";
            case EAssertionType::Assume:        return "ASSUME";
            case EAssertionType::DebugAssert:   return "DEBUG ASSERTION";
            case EAssertionType::Unreachable:   return "UNREACHABLE";
            case EAssertionType::Panic:         return "PANIC";
            case EAssertionType::Alert:         return "ALERT";
            default:                            return "";
        }
    }

    static void DefaultAssertionHandler(const FAssertion& Assertion)
    {
        if (::Lumina::Logging::IsInitialized())
        {
            LOG_CRITICAL("==================================================================================");
            LOG_CRITICAL("{} FAILED: {}", AssertionTypeToString(Assertion.Type), Assertion.Expression);
            
            if (!Assertion.Message.empty())
            {
                LOG_CRITICAL("Message: {}", Assertion.Message);
            }
            
            std::basic_stacktrace Trace = std::stacktrace::current();
            size_t Skip = 2;
            for (size_t i = Skip; i < Trace.size(); ++i)
            {
                const std::stacktrace_entry& Entry = Trace[i];
                LOG_CRITICAL("  #{} {}", i - Skip, std::to_string(Entry));
            }
            
            LOG_CRITICAL("File: {}:{}", Assertion.Location.file_name(), Assertion.Location.line());
            LOG_CRITICAL("Function: {}", Assertion.Location.function_name());
            
            LOG_CRITICAL("==================================================================================");
            Logging::Flush();
        }
    }
    
    static AssertionHandler GAssertionHandler = DefaultAssertionHandler;
    
    void Detail::HandleAssertion(const FAssertion& Assertion)
    {
        GAssertionHandler(Assertion);
    }

    [[noreturn]] static void ReportCheckFailure(const char* Expression, const char* Message,
        const std::source_location& Location)
    {
        LUMINA_DEBUG_BREAK();

        // The logger runs on these containers, so a check failing inside it would recurse through here forever.
        static std::atomic<bool> bReporting = false;
        if (bReporting.exchange(true, std::memory_order_relaxed))
        {
            Detail::Abort();
        }

        Detail::HandleAssertion(FAssertion
        {
            .Message = Message,
            .Location = Location,
            .Expression = Expression,
            .Type = EAssertionType::Assert
        });

        Detail::Abort();
    }

    void Detail::HandleCheckFailure(const char* Expression, const std::source_location& Location)
    {
        ReportCheckFailure(Expression, "", Location);
    }

    void Detail::HandleBoundsFailure(const char* IndexName, uint64 IndexValue,
        const char* Relation, const char* BoundName, uint64 BoundValue, const std::source_location& Location)
    {
        const FString ExpressionText = Format("{} {} {}", IndexName, Relation, BoundName);
        const FString MessageText = Format("{} is {}, {} is {}.", IndexName, IndexValue, BoundName, BoundValue);

        ReportCheckFailure(ExpressionText.c_str(), MessageText.c_str(), Location);
    }

    void Detail::Abort()
    {
        std::abort();
    }

    void SetAssertionHandler(AssertionHandler Handler)
    {
        GAssertionHandler = Handler ? Handler : DefaultAssertionHandler;
    }
}
