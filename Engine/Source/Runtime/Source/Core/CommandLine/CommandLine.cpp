#include "RuntimePCH.h"
#include <string>
#include "CommandLine.h"

namespace Lumina
{
    
    RUNTIME_API FCommandLine* GCommandLine = nullptr;
    
    namespace Detail
    {
        static FFixedString Normalize(FStringView Raw)
        {
            FFixedString String(Raw.begin(), Raw.end());
            String.ToLower();
            return Move(String);
        }
    }
    
    FCommandLine::FCommandLine(int argc, char* argv[])
    {
        Parse(argc, argv);
    }

    void FCommandLine::Parse(int argc, char* argv[])
    {
        for (int i = 1; i < argc; ++i)
        {
            FStringView Arg(argv[i], strlen(argv[i]));

            if (Arg.starts_with("--"))
            {
                // Lowercase the key but preserve the value's case (paths/identifiers).
                const FStringView Raw = Arg.substr(2);
                FFixedString Key;
                FFixedString Value;

                const size_t Equals = Raw.find('=');
                if (Equals != FStringView::npos)
                {
                    Key   = Detail::Normalize(Raw.substr(0, Equals));
                    // Passing 0 as the count used to silently empty the value, and the rest of the string is wanted.
                    const FStringView ValueView = Raw.substr(Equals + 1);
                    Value.assign(ValueView.data(), ValueView.size());
                }
                else
                {
                    Key = Detail::Normalize(Raw);
                    if (i + 1 < argc)
                    {
                        FStringView NextArg(argv[i + 1]);
                        if (!NextArg.starts_with("--"))
                        {
                            Value = argv[++i];
                        }
                    }
                }

                Args[Key] = Value;
            }
            else if (Arg.starts_with('-') && Arg.size() > 1)
            {
                for (size_t j = 1; j < Arg.size(); ++j)
                {
                    FFixedString Key(1, Arg[j]);
                    Args[Detail::Normalize(Key)] = "true";
                }
            }
            else
            {
                PositionalArgs.emplace_back(FFixedString(Arg.begin(), Arg.end()));
            }
        }
    }

    // Args is keyed by FName, and find is heterogeneous, so a raw string would be hashed as a string and miss.
    static FName LookupKey(const FString& Name)
    {
        return FName(Detail::Normalize(Name));
    }

    bool FCommandLine::Has(const FString& name) const
    {
        return Args.find(LookupKey(name)) != Args.end();
    }

    TOptional<FFixedString> FCommandLine::Get(const FString& Name) const
    {
        auto it = Args.find(LookupKey(Name));
        return it != Args.end() ? TOptional(it->second) : NullOpt;
    }

    TOptional<int> FCommandLine::GetInt(const FString& name) const
    {
        auto it = Args.find(LookupKey(name));
        return it != Args.end() ? TOptional(std::stoi(it->second.c_str())) : NullOpt;
    }

    TOptional<bool> FCommandLine::GetBool(const FString& name) const
    {
        auto it = Args.find(LookupKey(name));
        if (it == Args.end())
        {
            return NullOpt;
        }
    
        const FFixedString& Val = it->second;
        return Val.empty() || Val == "1" || Val == "true" || Val == "yes";
    }

    const TVector<FFixedString>& FCommandLine::GetPositionalArgs() const
    {
        return PositionalArgs;
    }

    const THashMap<FName, FFixedString>& FCommandLine::GetAll() const
    {
        return Args;
    }
}
