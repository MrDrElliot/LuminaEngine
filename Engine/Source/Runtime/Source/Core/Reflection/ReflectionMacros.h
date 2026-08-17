#pragma once

// Leaf header on purpose: Core/Math cannot reach Core/Object, so both include this and no cycle forms.

#define NO_API

#define CONCAT_INNER(a, b) a##b
#define CONCAT(a, b) CONCAT_INNER(a, b)

#define CONCAT3_INNER(a, b, c) a##b##c
#define CONCAT3(a, b, c) CONCAT3_INNER(a, b, c)

#define CONCAT4_INNER(a, b, c, d) a##b##c##d
#define CONCAT4(a, b, c, d) CONCAT4_INNER(a, b, c, d)

#define CONCAT_WITH_UNDERSCORE(a, b) CONCAT3(a, _, b)
#define FRIEND_STRUCT_NAME(ns, cls) CONCAT3(Construct_CClass_, CONCAT_WITH_UNDERSCORE(ns, cls), _Statics)

// libclang variadic macro expansion is unreliable during reflection parsing; stub these out then.
#if defined(REFLECTION_PARSER)

    #define GENERATED_BODY(...)
    #define REFLECT(...)
    #define PROPERTY(...)
    #define FUNCTION(...)
    // A marker only, detected via the macro record; it generates no code at the call site.
    #define SCRIPT_EXPORT(...)

#else

    #define GENERATED_BODY(...) CONCAT4(CURRENT_FILE_ID, _, __LINE__, _GENERATED_BODY)
    #define REFLECT(...)
    #define PROPERTY(...)
    #define FUNCTION(...)
    #define SCRIPT_EXPORT(...)

#endif
