// tests/test_main.cpp
//
// The unit test runner. Every KAP_TEST in any test_*.cpp translation unit has
// self-registered into the global registry by the time main() runs (the
// static Registrar objects in harness.hpp fire before main). We just iterate
// the registry, execute each body, and report.
//
// Exit code mirrors the test suite: 0 if every test passed, 1 otherwise, so
// `ctest --test-dir build` and the CI pipeline fail on any regression.

#include "harness.hpp"

#include <cstdio>
#include <exception>

int main()
{
    auto& tests = kap_test::registry();

    int passed = 0;
    int failed = 0;

    for (auto& test : tests) {
        try {
            test.body();
            std::printf("[PASS] %s\n", test.name.c_str());
            ++passed;
        }
        catch (const kap_test::AssertionFailure& e) {
            std::printf("[FAIL] %s\n  %s\n", test.name.c_str(), e.what());
            ++failed;
        }
        catch (const std::exception& e) {
            std::printf("[FAIL] %s\n  unexpected exception: %s\n", test.name.c_str(), e.what());
            ++failed;
        }
    }

    std::printf("\n%d tests: %d passed, %d failed\n", passed + failed, passed, failed);
    return failed == 0 ? 0 : 1;
}