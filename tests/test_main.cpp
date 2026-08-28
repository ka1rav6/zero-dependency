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
            continue;
        }
        catch (const kap_test::AssertionFailure& e) {
            // An assertion inside the test already knows its own file:line.
            std::printf("[FAIL] %s\n  %s\n", test.name.c_str(), e.what());
        }
        catch (const std::exception& e) {
            // The test threw where it did not expect to. Point at the test's
            // own declaration (recorded by KAP_TEST) so the reader can find it
            // without grepping for the name.
            std::printf("[FAIL] %s\n  %s:%d: unexpected exception: %s\n",
                        test.name.c_str(),
                        test.file.c_str(),
                        test.line,
                        e.what());
        }
        catch (...) {
            // Something not derived from std::exception escaped. Without this
            // clause it would propagate out of main and abort the process, so
            // the run would report nothing at all — not even the tests that had
            // already passed — and CI would show a bare non-zero exit code.
            std::printf("[FAIL] %s\n  %s:%d: unexpected non-standard exception\n",
                        test.name.c_str(),
                        test.file.c_str(),
                        test.line);
        }
        ++failed;
    }

    // A suite that registered nothing is almost always a build/link mistake
    // (a test_*.cpp dropped from CMakeLists), and reporting "0 failed" for it
    // would be a green light for an empty run.
    if (passed + failed == 0) {
        std::printf("no tests were registered - is a test_*.cpp missing from CMakeLists.txt?\n");
        return 1;
    }

    std::printf("\n%d tests: %d passed, %d failed\n", passed + failed, passed, failed);
    return failed == 0 ? 0 : 1;
}