// SPDX-License-Identifier: GPL-2.0-or-later

// json.h: thin C++ view over cJSON. Non-owning JsonValue, owning JsonDoc.
//
// Jellyfin returns PascalCase keys ("AccessToken", "ImageTags"). Every
// accessor here is null-safe: a missing key yields a default rather than a
// crash, so parsing a truncated or unexpected response degrades instead of
// taking the console down.

#pragma once

#include <string>
#include <vector>

struct cJSON;

class JsonValue {
public:
    JsonValue() = default;
    explicit JsonValue(const cJSON* node) : node_(node) {}

    bool valid() const { return node_ != nullptr; }
    explicit operator bool() const { return valid(); }

    // Object member lookup. Case-sensitive, as Jellyfin is consistent.
    JsonValue operator[](const char* key) const;

    // Array access. size() is 0 for non-arrays.
    int       size() const;
    JsonValue at(int index) const;

    // Iterating an array: for (JsonValue v : arr.items())
    std::vector<JsonValue> items() const;

    // Scalars. Each returns fallback when the node is missing or mistyped.
    std::string asString(const char* fallback = "") const;
    int         asInt(int fallback = 0) const;
    double      asDouble(double fallback = 0.0) const;
    bool        asBool(bool fallback = false) const;

    // Convenience: obj.str("Name") == obj["Name"].asString()
    std::string str(const char* key, const char* fallback = "") const;
    int         num(const char* key, int fallback = 0) const;
    bool        flag(const char* key, bool fallback = false) const;

private:
    const cJSON* node_ = nullptr;
};

// Owns the parsed tree. Move-only.
class JsonDoc {
public:
    JsonDoc() = default;
    explicit JsonDoc(const std::string& text);
    ~JsonDoc();

    JsonDoc(JsonDoc&& other) noexcept;
    JsonDoc& operator=(JsonDoc&& other) noexcept;
    JsonDoc(const JsonDoc&) = delete;
    JsonDoc& operator=(const JsonDoc&) = delete;

    bool      valid() const { return root_ != nullptr; }
    JsonValue root() const;

    // Same shorthand as JsonValue so callers can skip .root().
    JsonValue operator[](const char* key) const { return root()[key]; }

private:
    cJSON* root_ = nullptr;
};

// Escapes a string for embedding in a JSON string literal (no surrounding
// quotes added). Used when building small request bodies by hand.
std::string JsonEscape(const std::string& in);
