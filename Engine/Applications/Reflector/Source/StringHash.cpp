#include "StringHash.h"

#include <mutex>
#include <unordered_map>
#include "xxhash.h"

namespace Lumina
{
    namespace
    {
        // Every reflected header, type and property name lands here, so it is sized up front.
        constexpr size_t kInternReserve = 16u * 1024u;

        std::unordered_map<uint64_t, std::string>& InternTable()
        {
            static std::unordered_map<uint64_t, std::string> Table;
            return Table;
        }

        // Code generation runs a thread per header and every type lookup interns its name.
        std::mutex& InternMutex()
        {
            static std::mutex Mutex;
            return Mutex;
        }
    }

    void FStringHash::Initialize()
    {
        InternTable().reserve(kInternReserve);
    }

    void FStringHash::Shutdown()
    {
        InternTable().clear();
    }

    void FStringHash::Intern(std::string_view Str)
    {
        if (Str.empty())
        {
            return;
        }

        ID = XXH64(Str.data(), Str.size(), 0);

        const std::lock_guard<std::mutex> Lock(InternMutex());
        InternTable().try_emplace(ID, Str);
    }

    FStringHash::FStringHash(const char* Char)
    {
        if (Char != nullptr)
        {
            Intern(std::string_view(Char));
        }
    }

    FStringHash::FStringHash(std::string_view Str)
    {
        Intern(Str);
    }

    bool FStringHash::IsNone() const
    {
        const std::lock_guard<std::mutex> Lock(InternMutex());
        return !InternTable().contains(ID);
    }

    std::string FStringHash::ToString() const
    {
        const char* Text = c_str();
        return Text != nullptr ? std::string(Text) : std::string();
    }

    const char* FStringHash::c_str() const
    {
        if (ID == 0)
        {
            return nullptr;
        }

        // The table never erases, so a string stays put once interned and the pointer outlives the lock.
        const std::lock_guard<std::mutex> Lock(InternMutex());
        const auto Itr = InternTable().find(ID);
        return Itr != InternTable().end() ? Itr->second.c_str() : nullptr;
    }
}
