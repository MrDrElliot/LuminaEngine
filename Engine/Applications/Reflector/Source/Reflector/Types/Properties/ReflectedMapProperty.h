#pragma once
#include "ReflectedProperty.h"

namespace Lumina
{
    // Reflected THashMap<K,V>, or any map that normalizes onto one (TFixedHashMap). The associative analogue
    // of FReflectedArrayProperty: it emits a single ops forwarder returning GetMapOpsFor<Container>(). The Key
    // and Value inner FProperties are created and pushed separately by the visitor (as [Value, Key] before the
    // map) so the runtime's backward ReadMore=2 walk attaches them in Key-then-Value order.
    class FReflectedMapProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() override { return nullptr; }
        const char* GetPropertyParamType() const override { return "FMapPropertyParams"; }
        std::string_view GetLuaType() override { return std::string_view{}; }

        void AppendDefinition(Reflection::FCodeWriter& Writer) const override;

        bool HasAccessors() override;
        bool DeclareAccessors(Reflection::FCodeWriter& Writer, const std::string& FileID) override;
        bool DefineAccessors(Reflection::FCodeWriter& Writer, Reflection::FReflectedType* ReflectedType) override;
        bool GenerateLuaBinding(Reflection::FCodeWriter& Writer) override;

        std::string KeyTypeName;
        std::string ValueTypeName;
    };
}
