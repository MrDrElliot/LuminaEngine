#pragma once

#include "Core/Reflection/ReflectionMacros.h"
#include "Memory/Construct.h"
#include "ObjectCore.h"
#include "Lumina.h"

enum EInternal { EC_InternalUseOnlyConstructor };


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
        LUMINA_STATIC_HELPER(Lumina::CClass*) \
        { \
            StaticValue = GetPrivateStaticClass(); \
        } \
        return StaticValue; \
    } \
    inline static Lumina::FName StaticName() \
    { \
        static Lumina::FName StaticName(#TClass); \
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


// Every engine type named inside these macros is fully qualified: a reflected class may live in any
// namespace, or none, and unqualified names only resolve while the type happens to sit in Lumina.
#define DEFINE_CLASS_FACTORY(TClass) \
    static Lumina::CObject* __PlacementNew(void* Memory) \
    { \
        return Lumina::Memory::ConstructAt(static_cast<TClass*>(Memory)); \
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
            ::Construct_CClass_Lumina_##TBaseClass(); \
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
