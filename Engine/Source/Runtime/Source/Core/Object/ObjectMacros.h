#pragma once

#include "ObjectCore.h"
#include "Lumina.h"

enum EInternal { EC_InternalUseOnlyConstructor };


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
    #define REFLECT(...)    __attribute__((annotate(#__VA_ARGS__)))
    #define FUNCTION(...)   __attribute__((annotate(#__VA_ARGS__)))
    #define PROPERTY(...)   __attribute__((annotate(#__VA_ARGS__)))

    #define REFLECT(...)
    #define PROPERTY(...)
    #define FUNCTION(...)
    // Exposes a namespace-scope free function to C#: the Reflector emits a native extern "C" thunk + a C#
    // [NativeCall] binding into the class named by Class="Namespace.Class". A marker only (detected via the
    // macro record); generates no code at the call site.
    #define SCRIPT_EXPORT(...)

#else

    #define GENERATED_BODY(...) CONCAT4(CURRENT_FILE_ID, _, __LINE__, _GENERATED_BODY)
    #define REFLECT(...)
    #define PROPERTY(...)
    #define FUNCTION(...)
    #define SCRIPT_EXPORT(...)

#endif

#define LUMINA_PURE_VIRTUAL(...) { UNREACHABLE(); }

#define DECLARE_CLASS(TNamespace, TClass, TBaseClass, TPackage, TAPI) \
private: \
    friend struct FRIEND_STRUCT_NAME(TNamespace, TClass); \
    TNamespace::TClass& operator=(TNamespace::TClass&&); \
    TNamespace::TClass& operator=(const TNamespace::TClass&); \
    TAPI static Lumina::CClass* GetPrivateStaticClass(); \
public: \
    using ThisClass = TNamespace::TClass; \
    using Super = TBaseClass; \
    inline static Lumina::CClass* StaticClass() \
    { \
        LUMINA_STATIC_HELPER(CClass*) \
        { \
            StaticValue = GetPrivateStaticClass(); \
        } \
        return StaticValue; \
    } \
    inline static Lumina::FName StaticName() \
    { \
        static FName StaticName(#TClass); \
        return StaticName; \
    } \
    inline static const TCHAR* StaticPackage() \
    { \
        return TEXT(TPackage); \
    } \

#define DECLARE_SERIALIZER(TNamespace, TClass) \
    public: \
    friend Lumina::FArchive& operator << (Lumina::FArchive& Ar, TNamespace::TClass*& Res) \
    { \
        return Ar << (Lumina::CObject*&)Res; \
    } \
    private:


#define DEFINE_CLASS_FACTORY(TClass) \
    static CObject* __PlacementNew(void* Memory) \
    { \
        return new (Memory) TClass(); \
    }

#define IMPLEMENT_CLASS(TNamespace, TClass) \
    Lumina::FClassRegistrationInfo CONCAT4(Registration_Info_CClass_, TNamespace, _, TClass); \
    NO_API Lumina::CClass* TNamespace::TClass::GetPrivateStaticClass() \
    { \
        if (CONCAT4(Registration_Info_CClass_, TNamespace, _, TClass).InnerSingleton == nullptr) \
        { \
            Lumina::AllocateStaticClass( \
                TNamespace::TClass::StaticPackage(), \
                TEXT(#TClass), \
                &CONCAT4(Registration_Info_CClass_, TNamespace, _, TClass).InnerSingleton, \
                sizeof(TNamespace::TClass), \
                alignof(TNamespace::TClass), \
                &TNamespace::TClass::Super::StaticClass, \
                &TNamespace::TClass::__PlacementNew); \
        } \
        return CONCAT4(Registration_Info_CClass_, TNamespace, _, TClass).InnerSingleton; \
    }


/** Intrinsic class auto-register. */
#define IMPLEMENT_INTRINSIC_CLASS(TClass, TBaseClass, TAPI) \
    TAPI Lumina::CClass* Construct_CClass_Lumina_##TClass(); \
    extern Lumina::FClassRegistrationInfo Registration_Info_CClass_Lumina_##TClass; \
    struct Construct_CClass_Lumina_##TClass##_Statics \
    { \
        static Lumina::CClass* Construct() \
        { \
            extern TAPI Lumina::CClass* Construct_CClass_Lumina_##TBaseClass(); \
            Lumina::CClass* SuperClass = Construct_CClass_Lumina_##TBaseClass(); \
            Lumina::CClass* Class = Lumina::TClass::StaticClass(); \
            Lumina::CObjectForceRegistration(Class); \
            return Class; \
        } \
    }; \
    Lumina::CClass* Construct_CClass_Lumina_##TClass() \
    { \
        if(!Registration_Info_CClass_Lumina_##TClass.OuterSingleton) \
        { \
            Registration_Info_CClass_Lumina_##TClass.OuterSingleton = Construct_CClass_Lumina_##TClass##_Statics::Construct(); \
        } \
        return Registration_Info_CClass_Lumina_##TClass.OuterSingleton; \
    } \
    IMPLEMENT_CLASS(Lumina, TClass) \
    static Lumina::FRegisterCompiledInInfo AutoInitialize_##TClass(&Construct_CClass_Lumina_##TClass, Lumina::TClass::StaticPackage(), TEXT(#TClass));

#if USING(WITH_EDITOR)
    #define METADATA_PARAMS(X, Y) X, Y
#else
    #define METADATA_PARAMS(X, Y)
#endif

namespace LuminaAsserts_Private
{
    // sizeof()-via-overload to handle potential bitfield members.
    template <typename T>
    bool GetMemberNameCheckedJunk(const T&);
    template <typename T>
    bool GetMemberNameCheckedJunk(const volatile T&);
    template <typename R, typename ...Args>
    bool GetMemberNameCheckedJunk(R(*)(Args...));
}

/** Returns FName(MemberName) while statically asserting the member exists. */
#define GET_MEMBER_NAME_CHECKED(ClassName, MemberName) \
((void)sizeof(LuminaAsserts_Private::GetMemberNameCheckedJunk(((ClassName*)0)->MemberName)), FName(TEXT(#MemberName)))
