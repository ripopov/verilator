// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Minimal JSON reader for VDB companion documents
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2026 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "Json.h"

#include <cstdlib>
#include <stdexcept>

namespace vdb_index {

namespace {

class Parser {
public:
    Parser(std::string_view text, std::vector<MemberSpan>* spans)
        : m_text{text}
        , m_spans{spans} {}

    Json document() {
        skipSpace();
        Json value = parseValue(true);
        skipSpace();
        if (m_pos != m_text.size()) fail("trailing characters after document");
        return value;
    }

private:
    std::string_view m_text;
    size_t m_pos = 0;
    std::vector<MemberSpan>* m_spans;

    [[noreturn]] void fail(const char* what) const {
        throw std::runtime_error{std::string{"invalid JSON at byte "} + std::to_string(m_pos)
                                 + ": " + what};
    }
    char peek() const { return m_pos < m_text.size() ? m_text[m_pos] : '\0'; }
    void skipSpace() {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++m_pos;
        }
    }
    void expect(char c) {
        if (peek() != c) fail("unexpected character");
        ++m_pos;
    }
    void literal(std::string_view word) {
        if (m_text.substr(m_pos, word.size()) != word) fail("unknown literal");
        m_pos += word.size();
    }

    static void appendUtf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    unsigned hex4() {
        if (m_pos + 4 > m_text.size()) fail("truncated escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                fail("bad hex escape");
            }
        }
        return value;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (true) {
            if (m_pos >= m_text.size()) fail("unterminated string");
            const char c = m_text[m_pos++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_pos >= m_text.size()) fail("unterminated escape");
            const char e = m_text[m_pos++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned code = hex4();
                if (code >= 0xD800 && code < 0xDC00 && m_text.substr(m_pos, 2) == "\\u") {
                    m_pos += 2;
                    const unsigned low = hex4();
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, code);
                break;
            }
            default: fail("unknown escape");
            }
        }
    }

    Json parseNumber() {
        const size_t start = m_pos;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e'
                || c == 'E') {
                ++m_pos;
            } else {
                break;
            }
        }
        if (start == m_pos) fail("expected a number");
        Json value;
        value.kind = Json::Kind::Number;
        value.number
            = std::strtod(std::string{m_text.substr(start, m_pos - start)}.c_str(), nullptr);
        return value;
    }

    Json parseValue(bool top) {
        Json value;
        switch (peek()) {
        case '{': {
            value.kind = Json::Kind::Object;
            ++m_pos;
            skipSpace();
            if (peek() == '}') {
                ++m_pos;
                return value;
            }
            while (true) {
                skipSpace();
                const size_t keyStart = m_pos;
                std::string key = parseString();
                skipSpace();
                expect(':');
                skipSpace();
                Json member = parseValue(false);
                if (top && m_spans) m_spans->push_back(MemberSpan{key, keyStart, m_pos});
                value.members.push_back(Member{std::move(key), std::move(member)});
                skipSpace();
                if (peek() == ',') {
                    ++m_pos;
                    continue;
                }
                expect('}');
                return value;
            }
        }
        case '[': {
            value.kind = Json::Kind::Array;
            ++m_pos;
            skipSpace();
            if (peek() == ']') {
                ++m_pos;
                return value;
            }
            while (true) {
                skipSpace();
                value.items.push_back(parseValue(false));
                skipSpace();
                if (peek() == ',') {
                    ++m_pos;
                    continue;
                }
                expect(']');
                return value;
            }
        }
        case '"':
            value.kind = Json::Kind::String;
            value.string = parseString();
            return value;
        case 't':
            literal("true");
            value.kind = Json::Kind::Bool;
            value.boolean = true;
            return value;
        case 'f':
            literal("false");
            value.kind = Json::Kind::Bool;
            return value;
        case 'n': literal("null"); return value;
        default: return parseNumber();
        }
    }
};

}  // namespace

const Json* Json::get(std::string_view key) const {
    for (const Member& member : members) {
        if (member.key == key) return &member.value;
    }
    return nullptr;
}

const Json& Json::at(std::string_view key) const {
    static const Json s_null;
    const Json* found = get(key);
    return found ? *found : s_null;
}

Json parseJson(std::string_view text, std::vector<MemberSpan>* spans) {
    return Parser{text, spans}.document();
}

std::string quoteJson(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char* const hex = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[(c >> 4) & 0xF]);
                out.push_back(hex[c & 0xF]);
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace vdb_index
