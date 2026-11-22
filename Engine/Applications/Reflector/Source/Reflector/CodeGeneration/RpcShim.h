#pragma once

#include <string>
#include <vector>

#include "Reflector/Public/ReflectionModel.h"

namespace Lumina::Reflection::RpcShim
{
    // The reflected class Type's parent names, resolved against Type's namespace, or an empty view past the reflected roots.
    inline FTypeView ParentClass(const FTypeView& Type)
    {
        const std::string& Parent = Type.Parent();
        if (Parent.empty())
        {
            return {};
        }

        const bool bQualified = Parent.find("::") != std::string::npos;
        const std::string Qualified = (bQualified || Type.Namespace().empty()) ? Parent : (Type.Namespace() + "::" + Parent);
        const FTypeView Found = Type.GetModel().FindType(Qualified);
        return (Found && Found.Kind() == ETypeKind::Class && !(Found == Type)) ? Found : FTypeView{};
    }

    // Every FUNCTION(Rpc = ...) the class declares or inherits, one per name, the most derived declaration first.
    inline std::vector<FFunctionView> CollectFunctions(const FTypeView& Type)
    {
        std::vector<FFunctionView> Out;
        for (FTypeView Current = Type; Current; Current = ParentClass(Current))
        {
            for (const FFunctionView Function : Current.Functions())
            {
                if (!Function.HasMetadata("Rpc"))
                {
                    continue;
                }

                bool bShadowed = false;
                for (const FFunctionView& Seen : Out)
                {
                    bShadowed |= Seen.Name() == Function.Name();
                }
                if (!bShadowed)
                {
                    Out.push_back(Function);
                }
            }
        }
        return Out;
    }

    // The subclass whose overrides route each RPC, which the class's factory builds in place of the class itself.
    inline std::string ShimName(const FTypeView& Type)
    {
        return Type.DisplayName() + "__Rpc";
    }
}
