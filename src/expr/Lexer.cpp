#include "expr/Lexer.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace reforce::expr {

const char* tokName(Tok t) {
    switch (t) {
        case Tok::Number: return "number";
        case Tok::Ident: return "identifier";
        case Tok::LParen: return "(";
        case Tok::RParen: return ")";
        case Tok::LBrace: return "{";
        case Tok::RBrace: return "}";
        case Tok::Comma: return ",";
        case Tok::Plus: return "+";
        case Tok::Minus: return "-";
        case Tok::Star: return "*";
        case Tok::Slash: return "/";
        case Tok::Percent: return "%";
        case Tok::Caret: return "^";
        case Tok::CaretCaret: return "^^";
        case Tok::EqEq: return "==";
        case Tok::NotEq: return "!=";
        case Tok::Less: return "<";
        case Tok::Greater: return ">";
        case Tok::LessEq: return "<=";
        case Tok::GreaterEq: return ">=";
        case Tok::AndAnd: return "&&";
        case Tok::OrOr: return "||";
        case Tok::Amp: return "&";
        case Tok::Pipe: return "|";
        case Tok::Shl: return "<<";
        case Tok::Shr: return ">>";
        case Tok::Bang: return "!";
        case Tok::End: return "end of expression";
    }
    return "?";
}

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

}  // namespace

std::vector<Token> tokenize(std::string_view s, Diagnostics& diags, int line, const std::string& key) {
    std::vector<Token> out;
    const std::size_t n = s.size();
    std::size_t i = 0;
    auto peek = [&](std::size_t k) -> char { return i + k < n ? s[i + k] : '\0'; };

    while (i < n) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            ++i;
            continue;
        }
        // Comments inside an expression string.
        if (c == '/' && peek(1) == '/') {
            while (i < n && s[i] != '\n' && s[i] != '\r') ++i;
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            const auto end = s.find("*/", i + 2);
            i = end == std::string_view::npos ? n : end + 2;
            continue;
        }

        Token t;
        t.pos = int(i);

        // Numbers: 12  1.5  .5  5.  1e30  1.5e-3  0x1F
        if (isDigit(c) || (c == '.' && isDigit(peek(1)))) {
            std::string text;
            if (c == '0' && (peek(1) == 'x' || peek(1) == 'X') && std::isxdigit(static_cast<unsigned char>(peek(2)))) {
                std::size_t j = i + 2;
                while (j < n && std::isxdigit(static_cast<unsigned char>(s[j]))) ++j;
                t.kind = Tok::Number;
                t.number = double(std::strtoull(std::string(s.substr(i + 2, j - i - 2)).c_str(), nullptr, 16));
                t.text = std::string(s.substr(i, j - i));
                i = j;
                out.push_back(std::move(t));
                continue;
            }
            std::size_t j = i;
            while (j < n && isDigit(s[j])) text += s[j++];
            if (j < n && s[j] == '.') {
                text += s[j++];
                while (j < n && isDigit(s[j])) text += s[j++];
            }
            // Lenient: a second decimal point followed by digits (".5.5").
            // Drop the extra point and keep the digits (".55").
            if (j + 1 < n && s[j] == '.' && isDigit(s[j + 1])) {
                const std::size_t start = j;
                ++j;
                while (j < n && isDigit(s[j])) text += s[j++];
                diags.warn("malformed-number",
                           "number with two decimal points '" + std::string(s.substr(i, j - i)) +
                               "' read as " + text + " (at offset " + std::to_string(start) + ")",
                           line, key);
            }
            if (j < n && (s[j] == 'e' || s[j] == 'E')) {
                std::size_t k = j + 1;
                if (k < n && (s[k] == '+' || s[k] == '-')) ++k;
                if (k < n && isDigit(s[k])) {
                    text += 'e';
                    text += std::string(s.substr(j + 1, k - j - 1));
                    while (k < n && isDigit(s[k])) text += s[k++];
                    j = k;
                }
            }
            // Lenient: C-style suffix (1L, 2f) not followed by an identifier char.
            if (j < n && (s[j] == 'L' || s[j] == 'l' || s[j] == 'F' || s[j] == 'f') &&
                !(j + 1 < n && isIdentChar(s[j + 1]))) {
                diags.warn("numeric-suffix",
                           "C-style numeric suffix ignored in '" + std::string(s.substr(i, j + 1 - i)) + "'", line, key);
                ++j;
            }
            t.kind = Tok::Number;
            t.number = std::strtod(text.c_str(), nullptr);
            t.text = std::move(text);
            i = j;
            out.push_back(std::move(t));
            continue;
        }

        if (isIdentStart(c)) {
            std::size_t j = i;
            while (j < n && isIdentChar(s[j])) ++j;
            t.kind = Tok::Ident;
            t.text = std::string(s.substr(i, j - i));
            i = j;
            out.push_back(std::move(t));
            continue;
        }

        auto op2 = [&](char a, char b) { return c == a && peek(1) == b; };
        Tok k = Tok::End;
        int len = 1;
        if (op2('^', '^')) { k = Tok::CaretCaret; len = 2; }
        else if (op2('=', '=')) { k = Tok::EqEq; len = 2; }
        else if (op2('!', '=')) { k = Tok::NotEq; len = 2; }
        else if (op2('<', '=')) { k = Tok::LessEq; len = 2; }
        else if (op2('>', '=')) { k = Tok::GreaterEq; len = 2; }
        else if (op2('&', '&')) { k = Tok::AndAnd; len = 2; }
        else if (op2('|', '|')) { k = Tok::OrOr; len = 2; }
        else if (op2('<', '<')) { k = Tok::Shl; len = 2; }
        else if (op2('>', '>')) { k = Tok::Shr; len = 2; }
        else {
            switch (c) {
                case '(': k = Tok::LParen; break;
                case ')': k = Tok::RParen; break;
                case '{': k = Tok::LBrace; break;
                case '}': k = Tok::RBrace; break;
                case ',': k = Tok::Comma; break;
                case '+': k = Tok::Plus; break;
                case '-': k = Tok::Minus; break;
                case '*': k = Tok::Star; break;
                case '/': k = Tok::Slash; break;
                case '%': k = Tok::Percent; break;
                case '^': k = Tok::Caret; break;
                case '<': k = Tok::Less; break;
                case '>': k = Tok::Greater; break;
                case '&': k = Tok::Amp; break;
                case '|': k = Tok::Pipe; break;
                case '!': k = Tok::Bang; break;
                default: break;
            }
        }
        if (k == Tok::End) {
            char buf[48];
            const unsigned char uc = static_cast<unsigned char>(c);
            if (uc >= 0x20 && uc < 0x7f) std::snprintf(buf, sizeof buf, "unexpected character '%c'", c);
            else std::snprintf(buf, sizeof buf, "unexpected byte 0x%02X", uc);
            diags.error("bad-character", buf, line, key);
            ++i;
            continue;
        }
        t.kind = k;
        t.text = std::string(s.substr(i, std::size_t(len)));
        i += std::size_t(len);
        out.push_back(std::move(t));
    }
    Token end;
    end.kind = Tok::End;
    end.pos = int(n);
    out.push_back(end);
    return out;
}

}  // namespace reforce::expr
