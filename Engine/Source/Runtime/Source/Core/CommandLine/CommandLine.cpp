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

    namespace Detail
    {
        // A negative number is a value, so "--offset -5" reads -5 rather than an option named "5".
        static bool IsOptionToken(FStringView Token)
        {
            if (Token.size() < 2 || Token[0] != '-')
            {
                return false;
            }
            const char After = Token[1];
            return After != '.' && (After < '0' || After > '9');
        }
    }

    // One dash reads the same as two; expanding "-server" per character left multi-character options false.
    void FCommandLine::Parse(int argc, char* argv[])
    {
        for (int i = 1; i < argc; ++i)
        {
            FStringView Arg(argv[i], strlen(argv[i]));

            if (Detail::IsOptionToken(Arg))
            {
                const size_t Dashes = Arg.starts_with("--") ? 2 : 1;

                // Lowercase the key but preserve the value's case (paths/identifiers).
                const FStringView Raw = Arg.substr(Dashes);
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
                        if (!Detail::IsOptionToken(NextArg))
                        {
                            Value = argv[++i];
                        }
                    }
                }

                Args[Key] = Value;
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
