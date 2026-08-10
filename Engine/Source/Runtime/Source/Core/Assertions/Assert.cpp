#include "RuntimePCH.h"
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

    void Detail::Abort()
    {
        std::abort();
    }

    void SetAssertionHandler(AssertionHandler Handler)
    {
        GAssertionHandler = Handler ? Handler : DefaultAssertionHandler;
    }
}
