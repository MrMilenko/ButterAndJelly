// SPDX-License-Identifier: GPL-2.0-or-later
#include "json.h"

#include "cJSON.h"

#include <cstdio>

// ---------------------------------------------------------------- JsonValue

JsonValue JsonValue::operator[](const char* key) const
{
    if (!node_ || !key) return JsonValue();
    const cJSON* child = cJSON_GetObjectItemCaseSensitive(node_, key);
    return JsonValue(child);
}

int JsonValue::size() const
{
    if (!node_ || !cJSON_IsArray(node_)) return 0;
    return cJSON_GetArraySize(const_cast<cJSON*>(node_));
}

JsonValue JsonValue::at(int index) const
{
    if (!node_ || !cJSON_IsArray(node_)) return JsonValue();
    return JsonValue(cJSON_GetArrayItem(const_cast<cJSON*>(node_), index));
}

std::vector<JsonValue> JsonValue::items() const
{
    std::vector<JsonValue> out;
    if (!node_ || !cJSON_IsArray(node_)) return out;
    const cJSON* child = nullptr;
    cJSON_ArrayForEach(child, node_) {
        out.push_back(JsonValue(child));
    }
    return out;
}

std::string JsonValue::asString(const char* fallback) const
{
    if (node_ && cJSON_IsString(node_) && node_->valuestring) {
        return node_->valuestring;
    }
    return fallback ? std::string(fallback) : std::string();
}

int JsonValue::asInt(int fallback) const
{
    if (node_ && cJSON_IsNumber(node_)) return (int)node_->valuedouble;
    return fallback;
}

double JsonValue::asDouble(double fallback) const
{
    if (node_ && cJSON_IsNumber(node_)) return node_->valuedouble;
    return fallback;
}

bool JsonValue::asBool(bool fallback) const
{
    if (!node_) return fallback;
    if (cJSON_IsBool(node_))   return cJSON_IsTrue(node_);
    if (cJSON_IsNumber(node_)) return node_->valuedouble != 0.0;
    return fallback;
}

std::string JsonValue::str(const char* key, const char* fallback) const
{
    return (*this)[key].asString(fallback);
}

int JsonValue::num(const char* key, int fallback) const
{
    return (*this)[key].asInt(fallback);
}

bool JsonValue::flag(const char* key, bool fallback) const
{
    return (*this)[key].asBool(fallback);
}

// ------------------------------------------------------------------ JsonDoc

JsonDoc::JsonDoc(const std::string& text)
{
    if (!text.empty()) {
        root_ = cJSON_ParseWithLength(text.c_str(), text.size());
    }
}

JsonDoc::~JsonDoc()
{
    if (root_) cJSON_Delete(root_);
}

JsonDoc::JsonDoc(JsonDoc&& other) noexcept : root_(other.root_)
{
    other.root_ = nullptr;
}

JsonDoc& JsonDoc::operator=(JsonDoc&& other) noexcept
{
    if (this != &other) {
        if (root_) cJSON_Delete(root_);
        root_ = other.root_;
        other.root_ = nullptr;
    }
    return *this;
}

JsonValue JsonDoc::root() const
{
    return JsonValue(root_);
}

// ------------------------------------------------------------------- escape

std::string JsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\"";  break;
            case '\\': out += "\\\\";  break;
            case '\b': out += "\\b";   break;
            case '\f': out += "\\f";   break;
            case '\n': out += "\\n";   break;
            case '\r': out += "\\r";   break;
            case '\t': out += "\\t";   break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}
