// Lisp repl
#pragma once
#ifndef REPL_CPP_
#define REPL_CPP_
#include <memory>
#include <vector>
#include <string>
#include <variant>
#include <exception>
#include <optional>
#include <cctype>
#include <type_traits>
#include <algorithm>
#include <iostream>
#include "enum.h"
#include "json.hpp"
namespace repl {
    using ::nlohmann::json;
    struct variable {
        std::optional<std::string> name = std::nullopt;
        std::variant<std::monostate, double, std::string> value;
    };
    namespace libs {

    };
    std::vector<variable> global_variables;
    void to_json(json &j, const variable &p) {
        if (p.name.has_value()) {
            j = json{p.name.value()};
        } else {
            std::visit([&] (auto &&value) {
                using T = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    j = json{};
                } else if constexpr (std::is_same_v<T, double>) {
                    j = json{value};
                } else if constexpr (std::is_same_v<T, std::string>) {
                    j = json{"\"" + value + "\""};
                }
            }, p.value);
        }
    }
    void from_json(const json& j, variable &p) {
        if (j.is_null()) {
            return;
        } else if (j.is_number()) {
            p.value = j.get<double>();
        } else if (j.is_string()) {
            auto value = j.get<std::string>();
            if (value.at(0) == '"' && value.back() == '"') {
                p.value = value.substr(1, value.size() - 2);
            } else {
                p.name = value;
            }
        }
    }
    struct ast {
        std::shared_ptr<variable> value = nullptr;
        std::vector<std::unique_ptr<ast>> childs = {};
    };
    void to_json(json &j, const ast &p) {
        if (p.value) {
            j = json{*p.value};
        } else {
            j = json::array();
            std::transform(p.childs.begin(), p.childs.end(), std::back_inserter(j), [] (const auto &i) {
                return json{*i};
            });
        }
    }
    void from_json(const json& j, ast &p) {
        if (j.is_array()) {
            std::transform(j.begin(), j.end(), std::back_inserter(p.childs), [] (const json &i) {
                return std::make_unique<ast>(i.get<ast>());
            });
        } else {
            p.value = std::make_shared<variable>(j.get<variable>());
        }
    }
    namespace lexer {
        BETTER_ENUM(token_type, unsigned char, STRING, NUMBER, WORD, UNKNOWN)

        bool is_string(char c) {
            return c == '"';
        }

        bool is_operator(char c) {
            return c == '(' || c == ')' || c == ';';
        }

        std::optional<std::vector<std::string>> parse1(std::string str) noexcept {
            str += " ";
            if (str.empty()) {
                return std::nullopt;
            }
            std::string token;
            std::vector<std::string> tokens;
            auto type = +token_type::UNKNOWN;
            for (auto iter = str.begin(); iter != str.end(); ++iter) {
                char c = *iter;
                restart: switch (type) {
                    case token_type::NUMBER:
                        if (!(std::isdigit(c) || c == '.')) {
                            tokens.push_back(std::move(token));
                            token.clear();
                            type = token_type::UNKNOWN;
                            goto restart;
                        }
                        token += c;
                        break;
                    case token_type::STRING:
                        token += c;
                        if (c == '"') {
                            tokens.push_back(std::move(token));
                            token.clear();
                            type = token_type::UNKNOWN;
                        }
                        break;
                    case token_type::WORD:
                        if (std::isspace(c) || is_operator(c) || is_string(c)) {
                            tokens.push_back(std::move(token));
                            token.clear();
                            type = token_type::UNKNOWN;
                            goto restart;
                        }
                        token += c;
                        break;
                    case token_type::UNKNOWN:
                        if (std::isspace(c)) {
                            break;
                        }
                        if (is_operator(c)) {
                            // operators
                            tokens.push_back(std::string{c});
                            break;
                        }
                        if (is_string(c)) {
                            // string
                            type = token_type::STRING;
                            token.push_back(c);
                            break;
                        }
                        if (std::isdigit(c)) {
                            // number
                            type = token_type::NUMBER;
                            token.push_back(c);
                            break;
                        }
                        // word
                        type = token_type::WORD;
                        token.push_back(c);
                        break;
                }
            }
            return tokens;
        }
        std::optional<ast> parse2(const std::vector<std::string> &tokens) {
            ast root;
            std::vector<ast *> stack{&root};
            for (auto &&token : tokens) {
                if (token == "(") {
                    auto &childs = stack.back()->childs;
                    childs.push_back(std::make_unique<ast>());
                    stack.push_back(childs.back().get());
                } else if (token == ")") {
                    stack.pop_back();
                } else {
                    auto &last = *stack.back();
                    variable new_value;
                    if (std::isdigit(token[0])) {
                        new_value.value = std::stod(token);
                    } else if (token[0] == '"') {
                        new_value.value = token.substr(1, token.size() - 2);
                    } else {
                        new_value.name = token;
                    }
                    last.childs.push_back(std::make_unique<ast>(ast {
                        .value = std::make_shared<variable>(new_value)
                    }));
                }
            }
            root = std::move(*root.childs[0]);
            return root;
        }
    };
};
#endif