#pragma once

// A deliberately minimal, header-only test harness -- no external
// dependency to fetch, which matters for a portfolio project meant to
// build cleanly anywhere. TEST_CASE registers itself into a function-local
// static registry at static-init time (a Meyers-singleton pattern, so
// registration order across translation units can't cause the "static
// initialization order fiasco"); main.cpp just calls run_all().

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct AssertionFailure {
    std::string message;
};

inline int run_all() {
    int passed = 0, failed = 0;
    for (auto& t : registry()) {
        try {
            t.fn();
            std::cout << "[PASS] " << t.name << "\n";
            ++passed;
        } catch (const AssertionFailure& e) {
            std::cout << "[FAIL] " << t.name << ": " << e.message << "\n";
            ++failed;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << t.name << " (unexpected exception): " << e.what() << "\n";
            ++failed;
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, " << registry().size() << " total\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace testfw

#define TEST_CASE(name)                                                  \
    void name();                                                          \
    static testfw::Registrar registrar_##name(#name, name);              \
    void name()

#define CHECK(cond)                                                                          \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::ostringstream oss;                                                            \
            oss << "CHECK failed: " #cond " at " << __FILE__ << ":" << __LINE__;               \
            throw testfw::AssertionFailure{oss.str()};                                         \
        }                                                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                    \
        if (!((a) == (b))) {                                                                                \
            std::ostringstream oss;                                                                         \
            oss << "CHECK_EQ failed: " #a " == " #b " at " << __FILE__ << ":" << __LINE__                   \
                << "  (" << (a) << " != " << (b) << ")";                                                     \
            throw testfw::AssertionFailure{oss.str()};                                                       \
        }                                                                                                    \
    } while (0)
