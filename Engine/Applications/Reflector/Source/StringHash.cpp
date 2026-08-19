#include "StringHash.h"

#include <unordered_map>
#include "Reflector/Clang/Utils.h"


namespace Lumina
{
    class FNameHashMap : public std::unordered_map<uint64_t, std::string>
    {
    };

    FNameHashMap* gNameCache = nullptr;
    

    void FStringHash::Initialize()
    {
        gNameCache = new FNameHashMap();
    }

    void FStringHash::Shutdown()
    {
        delete gNameCache;
        gNameCache = nullptr;
    }

    FStringHash::FStringHash(const char* Char)
    {
        if (Char != nullptr && strlen(Char) > 0)
        {
            ID = ClangUtils::HashString(Char);

            auto Itr = gNameCache->find(ID);
            if (Itr == gNameCache->end())
            {
                (*gNameCache)[ID] = std::string(Char);
            }
        }
    }
    

    FStringHash::FStringHash(const std::string& Str)
        :FStringHash(Str.c_str())
    {
    }

    FStringHash::FStringHash(const std::string_view& Str)
        :FStringHash(Str.data())
    {
    }

    bool FStringHash::IsNone() const
    {
        auto Itr = gNameCache->find(ID);
        return Itr == gNameCache->end();
    }

    std::string FStringHash::ToString() const
    {
        return std::string(c_str());
    }

    const char* FStringHash::c_str() const
    {
        if (ID == 0)
        {
            return nullptr;
        }

        auto Itr = gNameCache->find(ID);
        if (Itr != gNameCache->end())
        {
            return Itr->second.c_str();
        }

        return nullptr;
    }
}
