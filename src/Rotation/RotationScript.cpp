/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "RotationScript.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace Playerbots
{
namespace
{
std::string_view Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

std::string Lower(std::string_view text)
{
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return result;
}

// ---- condition parser: recursive descent over a token list ----

struct Token
{
    enum class Type { End, Number, Name, Op, Paren } Kind = Type::End;
    std::string Text;
    double Number = 0.0;
};

bool Tokenize(std::string_view text, std::vector<Token>& tokens, std::string& error)
{
    size_t i = 0;
    while (i < text.size())
    {
        char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            ++i;
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]))))
        {
            size_t start = i;
            while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.'))
                ++i;

            // a name that starts with digits ("1234.remains") is not a number
            if (i < text.size() && (std::isalpha(static_cast<unsigned char>(text[i])) || text[i] == '_'))
            {
                while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_' || text[i] == '.' || text[i] == '\'' || text[i] == ':' || text[i] == '-'))
                    ++i;
                tokens.push_back({ Token::Type::Name, std::string(text.substr(start, i - start)), 0.0 });
                continue;
            }

            Token token;
            token.Kind = Token::Type::Number;
            token.Text = std::string(text.substr(start, i - start));
            auto [end, code] = std::from_chars(token.Text.data(), token.Text.data() + token.Text.size(), token.Number);
            if (code != std::errc() || end != token.Text.data() + token.Text.size())
            {
                error = "bad number '" + token.Text + "'";
                return false;
            }
            tokens.push_back(std::move(token));
            continue;
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            size_t start = i;
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_' || text[i] == '.' || text[i] == '\'' || text[i] == ':' || text[i] == '-'))
                ++i;
            tokens.push_back({ Token::Type::Name, std::string(text.substr(start, i - start)), 0.0 });
            continue;
        }

        if (c == '(' || c == ')')
        {
            tokens.push_back({ Token::Type::Paren, std::string(1, c), 0.0 });
            ++i;
            continue;
        }

        // operators
        for (std::string_view op : { "<=", ">=", "==", "!=", "&&", "||", "<", ">", "!" })
            if (text.substr(i, op.size()) == op)
            {
                tokens.push_back({ Token::Type::Op, std::string(op), 0.0 });
                i += op.size();
                goto next;
            }

        error = std::string("unexpected '") + c + "'";
        return false;
    next:;
    }
    return true;
}

class Parser
{
public:
    explicit Parser(std::vector<Token> const& tokens) : _tokens(tokens) { }

    std::unique_ptr<Expr> ParseAll(std::string& error)
    {
        std::unique_ptr<Expr> expr = ParseOr();
        if (!_error.empty())
        {
            error = _error;
            return nullptr;
        }

        if (_pos != _tokens.size())
        {
            error = "unexpected '" + _tokens[_pos].Text + "'";
            return nullptr;
        }
        return expr;
    }

private:
    bool IsOp(char const* text) const { return _pos < _tokens.size() && _tokens[_pos].Kind == Token::Type::Op && _tokens[_pos].Text == text; }
    bool IsParen(char c) const { return _pos < _tokens.size() && _tokens[_pos].Kind == Token::Type::Paren && _tokens[_pos].Text[0] == c; }

    std::unique_ptr<Expr> Fail(std::string message)
    {
        if (_error.empty())
            _error = std::move(message);
        return nullptr;
    }

    std::unique_ptr<Expr> Binary(Expr::Kind kind, std::unique_ptr<Expr> left, std::unique_ptr<Expr> right)
    {
        auto expr = std::make_unique<Expr>();
        expr->Type = kind;
        expr->Left = std::move(left);
        expr->Right = std::move(right);
        return expr;
    }

    std::unique_ptr<Expr> ParseOr()
    {
        std::unique_ptr<Expr> left = ParseAnd();
        while (left && IsOp("||"))
        {
            ++_pos;
            std::unique_ptr<Expr> right = ParseAnd();
            if (!right)
                return nullptr;
            left = Binary(Expr::Kind::Or, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expr> ParseAnd()
    {
        std::unique_ptr<Expr> left = ParseNot();
        while (left && IsOp("&&"))
        {
            ++_pos;
            std::unique_ptr<Expr> right = ParseNot();
            if (!right)
                return nullptr;
            left = Binary(Expr::Kind::And, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expr> ParseNot()
    {
        if (IsOp("!"))
        {
            ++_pos;
            std::unique_ptr<Expr> inner = ParseNot();
            if (!inner)
                return nullptr;
            auto expr = std::make_unique<Expr>();
            expr->Type = Expr::Kind::Not;
            expr->Left = std::move(inner);
            return expr;
        }
        return ParseComparison();
    }

    std::unique_ptr<Expr> ParseComparison()
    {
        std::unique_ptr<Expr> left = ParseAtom();
        if (!left)
            return nullptr;

        for (auto [text, op] : { std::pair<char const*, Expr::Op>{ "<=", Expr::Op::LessEqual }, { ">=", Expr::Op::GreaterEqual }, { "==", Expr::Op::Equal },
            { "!=", Expr::Op::NotEqual }, { "<", Expr::Op::Less }, { ">", Expr::Op::Greater } })
        {
            if (!IsOp(text))
                continue;

            ++_pos;
            std::unique_ptr<Expr> right = ParseAtom();
            if (!right)
                return nullptr;
            auto expr = Binary(Expr::Kind::Compare, std::move(left), std::move(right));
            expr->Comparison = op;
            return expr;
        }
        return left;
    }

    std::unique_ptr<Expr> ParseAtom()
    {
        if (_pos >= _tokens.size())
            return Fail("the condition ends too early");

        Token const& token = _tokens[_pos];
        if (token.Kind == Token::Type::Number)
        {
            ++_pos;
            auto expr = std::make_unique<Expr>();
            expr->Type = Expr::Kind::Number;
            expr->Value = token.Number;
            return expr;
        }

        if (token.Kind == Token::Type::Name)
        {
            ++_pos;
            auto expr = std::make_unique<Expr>();
            expr->Type = Expr::Kind::Fact;
            expr->Name = Lower(token.Text);
            return expr;
        }

        if (IsParen('('))
        {
            ++_pos;
            std::unique_ptr<Expr> inner = ParseOr();
            if (!inner)
                return nullptr;
            if (!IsParen(')'))
                return Fail("missing )");
            ++_pos;
            return inner;
        }

        return Fail("unexpected '" + token.Text + "'");
    }

    std::vector<Token> const& _tokens;
    size_t _pos = 0;
    std::string _error;
};

// A value of an expression: numbers are numbers, facts are numbers when they are number facts, flags are 1/0
bool ValueOf(Expr const& expr, FactSource const& facts, double& value)
{
    switch (expr.Type)
    {
        case Expr::Kind::Number:
            value = expr.Value;
            return true;
        case Expr::Kind::Fact:
        {
            if (facts.Number(expr.Name, value))
                return true;
            bool flag = false;
            if (facts.Flag(expr.Name, flag))
            {
                value = flag ? 1.0 : 0.0;
                return true;
            }
            return false;
        }
        default:
            value = Evaluate(expr, facts) ? 1.0 : 0.0;
            return true;
    }
}
}

std::unique_ptr<Expr> ParseCondition(std::string_view text, std::string& error)
{
    std::vector<Token> tokens;
    if (!Tokenize(text, tokens, error))
        return nullptr;

    if (tokens.empty())
    {
        error = "empty condition";
        return nullptr;
    }

    Parser parser(tokens);
    return parser.ParseAll(error);
}

bool Evaluate(Expr const& expr, FactSource const& facts)
{
    switch (expr.Type)
    {
        case Expr::Kind::Number:
            return expr.Value != 0.0;
        case Expr::Kind::Fact:
        {
            bool flag = false;
            if (facts.Flag(expr.Name, flag))
                return flag;
            double number = 0.0;
            return facts.Number(expr.Name, number) && number != 0.0;
        }
        case Expr::Kind::Not:
            return !Evaluate(*expr.Left, facts);
        case Expr::Kind::And:
            return Evaluate(*expr.Left, facts) && Evaluate(*expr.Right, facts);
        case Expr::Kind::Or:
            return Evaluate(*expr.Left, facts) || Evaluate(*expr.Right, facts);
        case Expr::Kind::Compare:
        {
            double left = 0.0;
            double right = 0.0;
            if (!ValueOf(*expr.Left, facts, left) || !ValueOf(*expr.Right, facts, right))
                return false;

            switch (expr.Comparison)
            {
                case Expr::Op::Less: return left < right;
                case Expr::Op::LessEqual: return left <= right;
                case Expr::Op::Greater: return left > right;
                case Expr::Op::GreaterEqual: return left >= right;
                case Expr::Op::Equal: return std::fabs(left - right) < 1e-9;
                case Expr::Op::NotEqual: return std::fabs(left - right) >= 1e-9;
            }
            return false;
        }
    }
    return false;
}

void CollectFacts(Expr const& expr, std::vector<std::string>& names)
{
    if (expr.Type == Expr::Kind::Fact)
        names.push_back(expr.Name);
    if (expr.Left)
        CollectFacts(*expr.Left, names);
    if (expr.Right)
        CollectFacts(*expr.Right, names);
}

ParseResult ParseRotations(std::string_view text, std::string const& fileName)
{
    ParseResult result;
    RotationSet* set = nullptr;
    RotationList* list = nullptr;
    uint32_t lineNumber = 0;

    auto error = [&](std::string const& message) { result.Errors.push_back(fileName + ":" + std::to_string(lineNumber) + ": " + message); };

    while (!text.empty())
    {
        size_t end = text.find('\n');
        std::string_view line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        ++lineNumber;

        // comments
        if (size_t hash = line.find('#'); hash != std::string_view::npos)
            line = Trim(line.substr(0, hash));
        if (line.empty())
            continue;

        if (line.starts_with("spec ") || line.starts_with("spec\t"))
        {
            std::string_view rest = Trim(line.substr(5));
            size_t space = rest.find_first_of(" \t");
            uint32_t id = 0;
            std::string_view number = rest.substr(0, space);
            auto [ptr, code] = std::from_chars(number.data(), number.data() + number.size(), id);
            if (code != std::errc() || ptr != number.data() + number.size() || !id)
            {
                error("spec needs a specialization id");
                set = nullptr;
                list = nullptr;
                continue;
            }

            result.Sets.push_back(std::make_unique<RotationSet>());
            set = result.Sets.back().get();
            set->Spec = id;
            set->Name = space == std::string_view::npos ? std::string() : std::string(Trim(rest.substr(space)));
            list = nullptr;
            continue;
        }

        if (line.back() == ':')
        {
            if (!set)
            {
                error("a list outside of a spec");
                continue;
            }

            std::string name = Lower(Trim(line.substr(0, line.size() - 1)));
            list = &set->Lists[name];
            list->Name = name;
            continue;
        }

        if (!list)
        {
            error("a spell outside of a list");
            continue;
        }

        RotationEntry entry;
        entry.Line = lineNumber;
        std::string_view spell = line;
        std::string_view options;
        if (size_t bar = line.find('|'); bar != std::string_view::npos)
        {
            spell = Trim(line.substr(0, bar));
            options = Trim(line.substr(bar + 1));
        }

        if (spell.empty())
        {
            error("a line without a spell");
            continue;
        }
        entry.Spell = std::string(spell);

        bool failed = false;
        while (!options.empty() && !failed)
        {
            size_t space = options.find_first_of(" \t");
            std::string_view token = options.substr(0, space);
            std::string_view rest = space == std::string_view::npos ? std::string_view() : Trim(options.substr(space));
            if (token == "if")
            {
                std::string message;
                entry.ConditionText = std::string(rest);
                entry.Condition = ParseCondition(rest, message);
                if (!entry.Condition)
                {
                    error("condition '" + entry.ConditionText + "': " + message);
                    failed = true;
                }
                break;
            }

            if (token.starts_with("target="))
                entry.Target = Lower(token.substr(7));
            else if (token == "offgcd")
                entry.OffGcd = true;
            else if (token == "ooc")
                entry.OutOfCombat = true;
            else
            {
                error("unknown option '" + std::string(token) + "'");
                failed = true;
            }

            options = rest;
        }

        if (!failed)
            list->Entries.push_back(std::move(entry));
    }

    return result;
}
}
