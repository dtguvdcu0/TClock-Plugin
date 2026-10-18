#include "json_strict.h"

#include <cwctype>

namespace tcalendar {
namespace {

class JsonParser {
public:
    explicit JsonParser(const std::wstring& text) : text_(text) {}

    bool ParseRootObject(JsonObject& out_object, std::wstring& out_error) {
        SkipWhitespace();
        JsonValue root;
        if (!ParseObject(root, out_error)) return false;
        SkipWhitespace();
        if (pos_ != text_.size()) {
            out_error = L"Trailing characters after root object";
            return false;
        }
        out_object = std::move(root.object_value);
        return true;
    }

private:
    const std::wstring& text_;
    size_t pos_ = 0;

    void SkipWhitespace() {
        while (pos_ < text_.size() && std::iswspace(text_[pos_])) {
            ++pos_;
        }
    }

    bool ParseValue(JsonValue& out_value, std::wstring& out_error) {
        SkipWhitespace();
        if (pos_ >= text_.size()) {
            out_error = L"Unexpected end of input";
            return false;
        }

        const wchar_t c = text_[pos_];
        if (c == L'{') {
            return ParseObject(out_value, out_error);
        }
        if (c == L'"') {
            out_value.type = JsonType::String;
            return ParseString(out_value.string_value, out_error);
        }
        if (MatchLiteral(L"true")) {
            out_value.type = JsonType::Bool;
            out_value.bool_value = true;
            return true;
        }
        if (MatchLiteral(L"false")) {
            out_value.type = JsonType::Bool;
            out_value.bool_value = false;
            return true;
        }
        if (MatchLiteral(L"null")) {
            out_value.type = JsonType::Null;
            return true;
        }

        out_error = L"Unsupported JSON token";
        return false;
    }

    bool ParseObject(JsonValue& out_value, std::wstring& out_error) {
        if (pos_ >= text_.size() || text_[pos_] != L'{') {
            out_error = L"Expected '{'";
            return false;
        }
        ++pos_;

        out_value.type = JsonType::Object;
        out_value.object_value.clear();

        SkipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == L'}') {
            ++pos_;
            return true;
        }

        while (true) {
            SkipWhitespace();
            std::wstring key;
            if (!ParseString(key, out_error)) return false;

            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != L':') {
                out_error = L"Expected ':' after object key";
                return false;
            }
            ++pos_;

            JsonValue value;
            if (!ParseValue(value, out_error)) return false;
            out_value.object_value[key] = std::move(value);

            SkipWhitespace();
            if (pos_ >= text_.size()) {
                out_error = L"Unexpected end of object";
                return false;
            }
            if (text_[pos_] == L'}') {
                ++pos_;
                return true;
            }
            if (text_[pos_] != L',') {
                out_error = L"Expected ',' or '}' in object";
                return false;
            }
            ++pos_;
        }
    }

    bool ParseString(std::wstring& out_string, std::wstring& out_error) {
        if (pos_ >= text_.size() || text_[pos_] != L'"') {
            out_error = L"Expected string";
            return false;
        }
        ++pos_;

        std::wstring result;
        while (pos_ < text_.size()) {
            const wchar_t c = text_[pos_++];
            if (c == L'"') {
                out_string = std::move(result);
                return true;
            }
            if (c == L'\\') {
                if (pos_ >= text_.size()) {
                    out_error = L"Invalid escape sequence";
                    return false;
                }
                const wchar_t esc = text_[pos_++];
                switch (esc) {
                    case L'"': result.push_back(L'"'); break;
                    case L'\\': result.push_back(L'\\'); break;
                    case L'/': result.push_back(L'/'); break;
                    case L'b': result.push_back(L'\b'); break;
                    case L'f': result.push_back(L'\f'); break;
                    case L'n': result.push_back(L'\n'); break;
                    case L'r': result.push_back(L'\r'); break;
                    case L't': result.push_back(L'\t'); break;
                    case L'u': {
                        wchar_t decoded = 0;
                        if (!ParseUnicodeEscape(decoded)) {
                            out_error = L"Invalid unicode escape";
                            return false;
                        }
                        result.push_back(decoded);
                        break;
                    }
                    default:
                        out_error = L"Unsupported escape sequence";
                        return false;
                }
                continue;
            }
            if (c < 0x20) {
                out_error = L"Control character in string";
                return false;
            }
            result.push_back(c);
        }

        out_error = L"Unterminated string";
        return false;
    }

    bool ParseUnicodeEscape(wchar_t& out_char) {
        if (pos_ + 4 > text_.size()) return false;
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const wchar_t c = text_[pos_++];
            value <<= 4;
            if (c >= L'0' && c <= L'9') value |= static_cast<unsigned>(c - L'0');
            else if (c >= L'a' && c <= L'f') value |= static_cast<unsigned>(10 + c - L'a');
            else if (c >= L'A' && c <= L'F') value |= static_cast<unsigned>(10 + c - L'A');
            else return false;
        }
        out_char = static_cast<wchar_t>(value);
        return true;
    }

    bool MatchLiteral(const wchar_t* literal) {
        const std::wstring lit(literal);
        if (pos_ + lit.size() > text_.size()) return false;
        if (text_.compare(pos_, lit.size(), lit) != 0) return false;
        pos_ += lit.size();
        return true;
    }
};

} // namespace

bool ParseJsonObject(const std::wstring& text, JsonObject& out_object, std::wstring& out_error) {
    JsonParser parser(text);
    return parser.ParseRootObject(out_object, out_error);
}

const JsonValue* FindField(const JsonObject& object, const wchar_t* key) {
    const auto it = object.find(key);
    if (it == object.end()) return nullptr;
    return &it->second;
}

bool GetStringField(const JsonObject& object, const wchar_t* key, std::wstring& out_value) {
    const JsonValue* field = FindField(object, key);
    if (!field || field->type != JsonType::String) return false;
    out_value = field->string_value;
    return true;
}

bool GetBoolField(const JsonObject& object, const wchar_t* key, bool& out_value) {
    const JsonValue* field = FindField(object, key);
    if (!field || field->type != JsonType::Bool) return false;
    out_value = field->bool_value;
    return true;
}

bool GetObjectField(const JsonObject& object, const wchar_t* key, const JsonObject*& out_object) {
    const JsonValue* field = FindField(object, key);
    if (!field || field->type != JsonType::Object) return false;
    out_object = &field->object_value;
    return true;
}

} // namespace tcalendar
