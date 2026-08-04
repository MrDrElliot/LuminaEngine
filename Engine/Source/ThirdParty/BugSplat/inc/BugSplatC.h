#pragma once

// BugSplatC.h - Flat C API for BugSplat crash reporting.
//
// This is a C-callable surface over the BugSplat C++ class (see BugSplat.h).
// Both APIs share a single implementation; use whichever fits your project:
//
//   - C++ projects linking the static library can use either header.
//     When using this header with the static library, define BUGSPLAT_STATIC.
//   - Projects linking the dynamic library (BugSplat.dll) should use this
//     header. Only the C ABI crosses the DLL boundary, so your runtime
//     library choice (/MT vs /MD) does not need to match BugSplat's.
//   - Foreign-language bindings (C#, Rust, Python, ...) should P/Invoke or
//     FFI these exports from BugSplat.dll.
//
// The C API manages a single process-wide BugSplat instance, created by
// BugSplat_Init. All strings are null-terminated UTF-16 (wchar_t).

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(BUGSPLAT_STATIC)
#define BUGSPLAT_API
#elif defined(BUGSPLAT_EXPORTS)
#define BUGSPLAT_API __declspec(dllexport)
#else
#define BUGSPLAT_API __declspec(dllimport)
#endif

#define BUGSPLAT_CALL __cdecl

// Initializes crash reporting and installs the unhandled exception filter.
// Call once, early in your application's lifetime. Returns 1 on success,
// 0 if already initialized or if any argument is null.
BUGSPLAT_API int BUGSPLAT_CALL BugSplat_Init(const wchar_t* database, const wchar_t* appName, const wchar_t* appVersion);

// Returns 1 if BugSplat_Init has been called successfully, 0 otherwise.
BUGSPLAT_API int BUGSPLAT_CALL BugSplat_IsInitialized(void);

// Sets crash 'key' field
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_SetKey(const wchar_t* key);

// Sets crash 'user' field default.  Crash dialog may override this value
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_SetUser(const wchar_t* user);

// Sets crash 'email' field default.  Crash dialog may override this value
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_SetEmail(const wchar_t* email);

// Sets crash 'userDescription' field default.  Crash dialog may override this value
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_SetUserDescription(const wchar_t* description);

// Sets initial value of 'notes' field.  BugSplat web app users can edit this field.
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_SetNotes(const wchar_t* notes);

// Sets a custom attribute included with crash reports
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_SetAttribute(const wchar_t* name, const wchar_t* value);

// Add/remove file attachments. Attachments are included in both crash reports
// and feedback uploads. Returns 1 on success, 0 on failure.
BUGSPLAT_API int BUGSPLAT_CALL BugSplat_AddAttachment(const wchar_t* filepath);
BUGSPLAT_API int BUGSPLAT_CALL BugSplat_RemoveAttachment(const wchar_t* filepath);

// When quiet is non-zero, the crash report dialog will not be presented to
// the user (desktop-only)
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_SetQuietMode(int quiet);

// Set the timeout in ms used to determine if a process is hung.
// Default is 5000.  Disable hang detection with 0.
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_SetHangDetectionTimeout(int ms);

// What the crash handler does after it creates and uploads the report.
typedef enum
{
    BUGSPLAT_CRASH_EXIT = 0,            // call exit() (default) - full CRT shutdown
    BUGSPLAT_CRASH_TERMINATE = 1,       // call TerminateProcess - hard kill, no CRT shutdown
    BUGSPLAT_CRASH_CONTINUE_SEARCH = 2  // return EXCEPTION_CONTINUE_SEARCH - hand off to the
                                        // OS's default handling (WER / debugger)
} BugSplat_CrashCompletion;

// Set the completion behavior. Default is BUGSPLAT_CRASH_EXIT. Use
// BUGSPLAT_CRASH_TERMINATE when the host's CRT shutdown can hang after a crash
// (e.g. a Unity player), or BUGSPLAT_CRASH_CONTINUE_SEARCH to let the OS's
// default unhandled-exception handling run after the report is sent.
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_SetCrashCompletionBehavior(BugSplat_CrashCompletion behavior);

// Override the BugSplat crash type id stamped on uploaded crashes. Default is 1
// (Native). Set to 15 (UnityNative) for Unity native crashes so the server applies
// the IL2CPP LineNumberMappings.json to symbolicate managed (C#) frames.
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_SetCrashType(int crashTypeId);

// Post all unsent crashes on a new thread. Returns 1 if posting started.
BUGSPLAT_API int BUGSPLAT_CALL BugSplat_PostAllCrashesAsync(void);

// Posts non-crashing user feedback (e.g. a bug report or feature request).
// The title is used as the stack key for grouping feedback in the dashboard.
// description may be NULL (treated as empty). Pass attachments as an array of
// attachmentCount file paths, or NULL/0 for none; these attachments are
// included only in this feedback upload and do not affect attachments added
// via BugSplat_AddAttachment. Returns 1 on success, 0 on failure.
//
// Note: the C++ BugSplat::PostFeedback takes a std::vector, which cannot cross
// the DLL boundary; this entry point takes a plain array so the C ABI stays
// CRT-agnostic.
BUGSPLAT_API int BUGSPLAT_CALL BugSplat_PostFeedback(const wchar_t* title,
                                                     const wchar_t* description,
                                                     const wchar_t* const* attachments,
                                                     int attachmentCount);

// Like BugSplat_PostFeedback, but also returns the BugSplat report id (crashId)
// and info URL for the submitted report so the caller can display it or link to
// it in the dashboard. Returns 1 on success, 0 on failure.
//
// outCrashId may be NULL if the id is not needed. To receive the info URL, pass
// a buffer in outInfoUrl and its capacity in wide characters (including the null
// terminator) in outInfoUrlChars; the URL is truncated if it does not fit and is
// always null-terminated. Pass NULL/0 to skip it. On failure outCrashId is set to
// 0 and outInfoUrl to an empty string.
//
// Note: the C++ BugSplat::PostFeedbackWithResult returns a FeedbackResult holding
// a std::wstring, which cannot cross the DLL boundary; this entry point uses a
// caller-supplied buffer so the C ABI stays CRT-agnostic.
BUGSPLAT_API int BUGSPLAT_CALL BugSplat_PostFeedbackWithResult(const wchar_t* title,
                                                               const wchar_t* description,
                                                               const wchar_t* const* attachments,
                                                               int attachmentCount,
                                                               int* outCrashId,
                                                               wchar_t* outInfoUrl,
                                                               int outInfoUrlChars);

// Sends an XML crash report to BugSplat, bypassing minidump creation. Program
// execution continues normally after this call (it does not terminate the
// process). See the SDK samples for the expected XML schema.
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_CreateXmlReport(const wchar_t* xmlReport);

// Sends an AddressSanitizer (ASan) report to BugSplat. The report is ASCII/UTF-8
// text, hence char rather than wchar_t.
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_CreateAsanReport(const char* asanReport);

// Sets the minidump type used for automatically captured crashes. dumpType is a
// combination of Windows MINIDUMP_TYPE flags (declared in DbgHelp.h).
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_SetMiniDumpType(int dumpType);

// Generates a crash report from caller-supplied exception information without
// terminating the process. exceptionPointers must be an EXCEPTION_POINTERS* as
// the OS provides inside an SEH __except filter (GetExceptionInformation()) or an
// unhandled-exception-filter callback - it is not a value you construct. Pass a
// combination of Windows MINIDUMP_TYPE flags for dumpType, or a negative value to
// use the SDK default. Most callers do not need this: the filter installed by
// BugSplat_Init already captures unhandled crashes automatically.
BUGSPLAT_API void BUGSPLAT_CALL BugSplat_GenerateDump(void* exceptionPointers, int dumpType);

#ifdef __cplusplus
}
#endif
