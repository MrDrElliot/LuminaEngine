#pragma once
#include "ObjectCore.h"
#include "Containers/Vector.h"

namespace Lumina
{
    
    template<typename T>
    class TDeferredRegistry
    {
    public:
        
        using TType = T::TType;

        static TDeferredRegistry& Get()
        {
            static TDeferredRegistry Registry;
            return Registry;
        }

        struct FRegistrant
        {
            TType* (*RegisterFunc)();
        };


        void AddRegistration(TType* (*RegisterFunc)())
        {
            Registrations.emplace_back(RegisterFunc);
        }

        template<typename FuncType>
        void ProcessRegistrations(FuncType&& OnRegistration)
        {
            size_t Num = Registrations.size();
            for (size_t Index = ProcessedRegistrations; Index < Num; ++Index)
            {
                TType* Object = Registrations[Index].RegisterFunc();
                OnRegistration(*Object);
            }
            
            ProcessedRegistrations = Num;
        }
        
        void ProcessRegistrations()
        {
            size_t Num = Registrations.size();
            for (size_t Index = ProcessedRegistrations; Index < Num; ++Index)
            {
                Registrations[Index].RegisterFunc();
            }
            
            ProcessedRegistrations = Num;
        }

        bool HasPendingRegistrations()
        {
            return Registrations.size() != ProcessedRegistrations;
        }

        size_t NumRegistrations() const { return Registrations.size(); }

        /** Drops registrations queued since a snapshot.
         *
         *  For unwinding a module whose DLL was loaded -- which runs its static registrars -- and then
         *  REFUSED. Every queued RegisterFunc points into that DLL, so freeing it while they are still
         *  queued means the next ProcessRegistrations() calls into unmapped memory.
         */
        void TruncateRegistrations(size_t NewNum)
        {
            if (NewNum >= Registrations.size())
            {
                return;
            }

            // Never below the processed watermark: those already ran, and their types are live in the
            // reflection system. Anything queued after it is strictly the refused module's tail.
            ASSERT(NewNum >= ProcessedRegistrations);
            Registrations.resize(NewNum);
        }

    private:
        
        TFixedVector<FRegistrant, 2024>     Registrations;
        size_t                              ProcessedRegistrations = 0;
        
    };

    using FClassDeferredRegistry = TDeferredRegistry<FClassRegistrationInfo>;
    using FEnumDeferredRegistry = TDeferredRegistry<FEnumRegistrationInfo>;
    using FStructDeferredRegistry = TDeferredRegistry<FStructRegistrationInfo>;
    
}
