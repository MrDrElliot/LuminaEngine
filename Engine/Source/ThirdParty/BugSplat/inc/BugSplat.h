#pragma once
#include <new.h>
#include <string>
#include <signal.h>
#include <vector>
#include <wtypes.h>
#include <DbgHelp.h>

class BugSplatImpl;

// Result of a user feedback submission via PostFeedbackWithResult.
// When success is true, crashId/infoUrl identify the report in the BugSplat dashboard.
// On failure crashId is 0 and infoUrl is empty.
struct FeedbackResult {
    bool success = false;
    int crashId = 0;
    std::wstring infoUrl;
};

// Determines what the crash handler does after it has created and uploaded the
// crash report. See BugSplat::SetCrashCompletionBehavior.
enum class BugSplatCrashCompletion
{
    Exit = 0,            // call exit() (default) - runs full CRT shutdown
    Terminate = 1,       // call TerminateProcess - hard kill, skips CRT shutdown
    ContinueSearch = 2   // return EXCEPTION_CONTINUE_SEARCH - hand off to the OS's
                         // default unhandled-exception handling (WER / debugger)
};

class BugSplat {

public:

    BugSplat(const wchar_t* database, 
             const wchar_t* appName, 
             const wchar_t* appVersion, 
             LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter = nullptr);
    ~BugSplat();

	// Returns true if Windows Error Reporting integration is enabled
	bool IsWerEnabled();

    // These methods can be used to modify guard memory after class initialization.
    void AllocGuardMemory( size_t nbytes );
    void FreeGuardMemory();

    // When true, the crash report dialog will not be presented to the user (desktop-only)
    void SetQuietMode(bool flag);

    // Sets crash 'key' field
    void SetKey(const wchar_t* user);

    // Sets crash 'user' field default.  Crash dialog may override this value
    void SetUser(const wchar_t* user);

    // Sets crash 'email' field default.  Crash dialog may override this value
    void SetEmail(const wchar_t* user);

    // Sets crash 'userDescription' field default.  Crash dialog may override this value
    void SetUserDescription(const wchar_t* user);

    // Sets initial value of 'notes' field.  BugSplat web app users can edit this field.
    void SetNotes(const wchar_t* user);

    void SetAttribute(const wchar_t* name, const wchar_t* value);

    void SetMiniDumpType(MINIDUMP_TYPE dumpType);

	//! Set the timeout in ms used to determine if a process is hung. Default is 5000.  Disable hang detection with 0.
    void SetHangDetectionTimeout(int ms);

    //! Determines what the crash handler does after creating and uploading the report:
    //! Exit (default, full CRT shutdown), Terminate (hard kill - use when the host's CRT
    //! shutdown can hang after a crash, e.g. a Unity player), or ContinueSearch (return
    //! EXCEPTION_CONTINUE_SEARCH so the OS's default handling or an attached debugger runs).
    void SetCrashCompletionBehavior(BugSplatCrashCompletion behavior);

    //! Override the crash type id stamped on uploaded crashes (default Native=1). Set to
    //! 15 (UnityNative) for Unity native crashes so the server applies LineNumberMappings.json.
    void SetCrashType(int crashTypeId);

    // Generates a BugSplat crash report.
    // Note, the Xbox team requires that MiniDumpFilterTriage be used for production
    void GenerateDump(LPEXCEPTION_POINTERS const exceptionPointers, 
                      MINIDUMP_TYPE dumpType = (MINIDUMP_TYPE)(MiniDumpNormal|MiniDumpFilterTriage)) const;
    
    // Add/remove file attachments. Attachments are included in both crash reports and feedback uploads.
    bool AddAttachment(const wchar_t* filepath);
    bool RemoveAttachment(const wchar_t* filepath);
    void ClearAttachments();

    // Sends an xml report to BugSplat, bypassing minidump creation.  
    // This function does not exit, normal program flow continues.
    // See myGdkCrasher.cpp for an example of the xml schema
    void CreateXmlReport(const wchar_t* xmlReport);

    void CreateAsanReport(const char* asanReport);

    // Sends user feedback to BugSplat. Returns true on success.
    // Title becomes the stack key for grouping feedback in the dashboard.
    // If attachments are provided, they are included only in this feedback upload
    // and automatically removed afterward. Existing crash report attachments are not affected.
    // This is a thin wrapper over PostFeedbackWithResult that discards the report id.
    bool PostFeedback(const wchar_t* title, const wchar_t* description = L"", const std::vector<const wchar_t*>& attachments = {});

    // Like PostFeedback, but returns the BugSplat report id (crashId) and infoUrl for the
    // submitted feedback so callers can display or link to the report.
    FeedbackResult PostFeedbackWithResult(const wchar_t* title, const wchar_t* description = L"", const std::vector<const wchar_t*>& attachments = {});

    // Returns folder current crash artifacts will use e.g. R:\\BugSplat\{uniq-guid-string}
    const wchar_t* GetCrashFolder();      
    void SetSuspendingState(BOOL status);
      
    // Post a single crash report, removes folder after successful upload
    void PostCrash();

    // Call only on a new thread.  Returns true if any crashes posted.
    bool PostAllCrashes();

    // Post all crashes on a new thread.  Always returns true
    bool PostAllCrashesAsync();

    // Remove crash folder and its contents
    void CleanupExceptionSystem();
     
    const wchar_t* GetLogFilePath();

private:
    BugSplatImpl* impl=nullptr;
};

//
// Helper functions used to set CRT state.
//
inline void terminator() { int* z = 0; *z = 13; }
inline void signal_handler(int) { terminator(); }
inline void __cdecl invalid_parameter_handler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
    terminator();
}
inline int memory_depleted(size_t)
{
    terminator();
    return 0;
}

// This call should be made once from your app to enable collection of certain CRT exceptions
inline void SetGlobalCRTExceptionBehavior()
{
    // There is a single set_terminate handler for all dynamically linked DLLs or EXEs; 
    // even if you call set_terminate your handler may be replaced by another, 
    // or you may be replacing a handler set by another DLL or EXE.
    // See https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/set-terminate-crt?view=vs-2019
    set_terminate(&terminator);

    // Because there is only one _purecall_handler for each process, when you call _set_purecall_handler 
    // it immediately impacts all threads. The last caller on any thread sets the handler.
    // See https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/get-purecall-handler-set-purecall-handler?view=vs-2019
    _set_purecall_handler(&terminator);

    // Only one function can be specified as the global invalid argument handler at a time. 
    // See https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/set-invalid-parameter-handler-set-thread-local-invalid-parameter-handler?view=vs-2019
    _set_invalid_parameter_handler(&invalid_parameter_handler);

    // There is a single _set_new_handler handler for all dynamically linked DLLs or executables; 
    // even if you call _set_new_handler your handler might be replaced by another or that you are 
    // replacing a handler set by another DLL or executable.
    // See https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/set-new-handler?view=vs-2019
    _set_new_handler(&memory_depleted);
    _set_new_mode(1);
}

// This call should be made in each thread of your application to enable collection of certain CRT exceptions
inline void SetPerThreadCRTExceptionBehavior()
{
    // Signal handling, required for each thread, at least for SIGABRT
    signal(SIGABRT, signal_handler);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

