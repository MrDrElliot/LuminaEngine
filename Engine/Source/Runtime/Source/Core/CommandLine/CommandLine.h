#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Core/Templates/Optional.h"

namespace Lumina
{
    RUNTIME_API extern class FCommandLine* GCommandLine;
    
    class RUNTIME_API FCommandLine
    {
    public:
    
        FCommandLine() = default;
    
        FCommandLine(int argc, char* argv[]);
    
        void Parse(int argc, char* argv[]);
    
        NODISCARD bool Has(const FString& name) const;
    
        NODISCARD TOptional<FFixedString> Get(const FString& Name) const;
    
        NODISCARD TOptional<int> GetInt(const FString& name) const;
    
        NODISCARD TOptional<bool> GetBool(const FString& name) const;
    
        const TVector<FFixedString>& GetPositionalArgs() const;

        const THashMap<FName, FFixedString>& GetAll() const;
    
    private:
    
        THashMap<FName, FFixedString>   Args;
        TVector<FFixedString>           PositionalArgs;
    };

}
