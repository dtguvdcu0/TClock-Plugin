#pragma once

#include <string>
#include <unordered_map>

namespace tcalendar {

enum class JsonType {
    Null,
    Bool,
    String,
    Object,
};

struct JsonValue {
    JsonType type = JsonType::Null;
    bool bool_value = false;
    std::wstring string_value;
    std::unordered_map<std::wstring, JsonValue> object_value;
};

using JsonObject = std::unordered_map<std::wstring, JsonValue>;

bool ParseJsonObject(const std::wstring& text, JsonObject& out_object, std::wstring& out_error);

const JsonValue* FindField(const JsonObject& object, const wchar_t* key);
bool GetStringField(const JsonObject& object, const wchar_t* key, std::wstring& out_value);
bool GetBoolField(const JsonObject& object, const wchar_t* key, bool& out_value);
bool GetObjectField(const JsonObject& object, const wchar_t* key, const JsonObject*& out_object);

} // namespace tcalendar
