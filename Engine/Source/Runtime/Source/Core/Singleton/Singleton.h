#pragma once

template<typename T>
class TSingleton
{
public:
    friend T;

    //@ TODO Horrible, just awful. Remove entirely.
    static T& Get()
    {
        static T Instance;
        return Instance;
    }

private:

    TSingleton() = default;
    TSingleton(const TSingleton&) = delete;
    TSingleton& operator=(const TSingleton&) = delete;
};

