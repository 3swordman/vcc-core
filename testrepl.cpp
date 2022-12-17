#include "repl.cpp"
#include <iostream>
int main(int argc, char *argv[]) {
    if (argc != 2) return 1;
    auto result1_opt = repl::lexer::parse1(argv[1]);
    if (!result1_opt.has_value()) return 1;
    auto result1 = result1_opt.value();
    for (auto &&i : result1) {
        std::cout << i << " ";
    }
    std::cout << std::endl;
    auto result2_opt = repl::lexer::parse2(result1);
    if (!result2_opt.has_value()) return 1;
    auto &result2 = result2_opt.value();
    std::cout << nlohmann::json{result2}.dump() << std::endl;
}