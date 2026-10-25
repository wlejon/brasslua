#include <iostream>

namespace brasslua::tests {
int runLexerParserTests();
int runRuntimeJitTests();
int runLuaOracleTests();
} // namespace brasslua::tests

int main() {
    int failures = 0;
    failures += brasslua::tests::runLexerParserTests();
    failures += brasslua::tests::runRuntimeJitTests();
    failures += brasslua::tests::runLuaOracleTests();

    if (failures == 0) {
        std::cout << "[PASS] All brasslua unit and differential oracle tests passed!\n";
        return 0;
    }
    std::cerr << "[FAIL] " << failures << " test(s) failed.\n";
    return 1;
}
