#pragma once

namespace Lumina
{

    class RUNTIME_API INonCopyable
    {
    public:
        INonCopyable() = default;
        INonCopyable(const INonCopyable&) = delete;
        INonCopyable& operator = (const INonCopyable&) = delete;
        
        INonCopyable(INonCopyable&&) = default;
        INonCopyable& operator=(INonCopyable&&) = default;
    };
    
}