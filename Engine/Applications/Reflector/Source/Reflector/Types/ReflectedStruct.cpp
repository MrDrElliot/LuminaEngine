#include "ReflectedType.h"

#include "Reflector/CodeGeneration/ReflectionNames.h"

namespace Lumina::Reflection
{

    FReflectedStruct::~FReflectedStruct() = default;

    void FReflectedStruct::PushProperty(std::unique_ptr<FReflectedProperty>&& NewProperty)
    {
        NewProperty->Outer = Names::OffsetBaseTypeName(EmittedCppQualifiedName(), Namespace, DisplayName);
        Props.push_back(std::move(NewProperty));
    }

    void FReflectedStruct::PushFunction(std::unique_ptr<FReflectedFunction>&& NewFunction)
    {
        NewFunction->Outer = Namespace.empty() ? DisplayName : (Namespace + "::" + DisplayName);
        Functions.push_back(std::move(NewFunction));
    }

    std::vector<std::unique_ptr<FReflectedProperty>> FReflectedStruct::TakeBackPropertiesFrom(size_t FirstIndex)
    {
        std::vector<std::unique_ptr<FReflectedProperty>> Taken;
        if (FirstIndex >= Props.size())
        {
            return Taken;
        }

        Taken.reserve(Props.size() - FirstIndex);
        for (size_t Index = FirstIndex; Index < Props.size(); ++Index)
        {
            Taken.push_back(std::move(Props[Index]));
        }
        Props.erase(Props.begin() + (std::ptrdiff_t)FirstIndex, Props.end());

        return Taken;
    }
}
