#pragma once
#include "ReflectedProperty.h"

namespace Lumina
{
    // Reflected THashMap<K,V>. The associative analogue of FReflectedArrayProperty: it emits a single ops
    // forwarder returning GetMapOps<Key,Value>() and carries both template type names. The Key and Value inner
    // FProperties are created and pushed separately by the visitor (as [Value, Key] before the map) so the
    // runtime's backward ReadMore=2 walk attaches them in Key-then-Value order.
    class FReflectedMapProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() override { return nullptr; }
        const char* GetPropertyParamType() const override { return "FMapPropertyParams"; }
        eastl::string_view GetLuaType() override { return eastl::string_view{}; }

        void AppendDefinition(Reflection::FCodeWriter& Writer) const override;

        bool HasAccessors() override;
        bool DeclareAccessors(Reflection::FCodeWriter& Writer, const eastl::string& FileID) override;
        bool DefineAccessors(Reflection::FCodeWriter& Writer, Reflection::FReflectedType* ReflectedType) override;
        bool GenerateLuaBinding(Reflection::FCodeWriter& Writer) override;

        eastl::string KeyTypeName;
        eastl::string ValueTypeName;
    };
}
