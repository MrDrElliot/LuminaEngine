// X-macro list of reflected property types: the single source for EPropertyTypeFlags and its name tables
// (expanded in ObjectCore.h). Order IS the ABI: the enum values are the wire kind bytes the C# script schema
// sends (LuminaSharp.EPropertyType mirrors this, validated at bootstrap), so only ever APPEND here.
//
// NOTE: the Reflector keeps a deliberately-isolated parallel copy in Reflector/Types/PropertyFlags.h; it cannot
// include runtime headers, so it must be kept in the same order by hand.
//
// Usage: #define LE_PROPERTY_TYPE(Name) ... then #include this, then #undef. 'None' (0) and 'Count' are added
// by the includer around this list.

LE_PROPERTY_TYPE(Int8)
LE_PROPERTY_TYPE(Int16)
LE_PROPERTY_TYPE(Int32)
LE_PROPERTY_TYPE(Int64)

LE_PROPERTY_TYPE(UInt8)
LE_PROPERTY_TYPE(UInt16)
LE_PROPERTY_TYPE(UInt32)
LE_PROPERTY_TYPE(UInt64)

LE_PROPERTY_TYPE(Float)
LE_PROPERTY_TYPE(Double)

LE_PROPERTY_TYPE(Bool)
LE_PROPERTY_TYPE(Object)
LE_PROPERTY_TYPE(SoftObject)
LE_PROPERTY_TYPE(Class)
LE_PROPERTY_TYPE(Name)
LE_PROPERTY_TYPE(String)
LE_PROPERTY_TYPE(Enum)
LE_PROPERTY_TYPE(Vector)
LE_PROPERTY_TYPE(Struct)
LE_PROPERTY_TYPE(Optional)
LE_PROPERTY_TYPE(SubStruct)
LE_PROPERTY_TYPE(Delegate)
LE_PROPERTY_TYPE(InstancedStruct)
LE_PROPERTY_TYPE(Map)
