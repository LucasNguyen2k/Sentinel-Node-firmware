// minitest.h - tiny single-header test framework for the firmware host tests.
// Each TEST carries the firmware requirements it verifies (FW-xx), which end up in the JUnit XML
// (for Jenkins) and in a JSON file used by ci/trace_report.py.
//
//   TEST(gas_alarm_hysteresis, "FW-04") { ... CHECK(x); CHECK_EQ(a, b); ... }
//
// Run:  ./test_x --suite name --junit out.xml --json out.json
#pragma once
#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace minitest {

struct Case {
    const char *name;
    const char *reqs;
    void (*fn)();
};

inline std::vector<Case> &registry()
{
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char *n, const char *r, void (*f)()) { registry().push_back({n, r, f}); }
};

inline std::vector<std::string> &failures()
{
    static std::vector<std::string> f;
    return f;
}

template <typename T>
std::string show(const T &v)
{
    std::ostringstream o;
    o << v;
    return o.str();
}
inline std::string show(const std::string &v) { return "\"" + v + "\""; }
inline std::string show(const char *v) { return v ? "\"" + std::string(v) + "\"" : "null"; }

inline void fail(const char *file, int line, const std::string &msg)
{
    std::ostringstream o;
    o << file << ":" << line << ": " << msg;
    failures().push_back(o.str());
}

inline std::string xml(const std::string &s)
{
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

inline int run(int argc, char **argv)
{
    std::string suite = "fw_host", junit, json;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--suite")) suite = argv[++i];
        else if (!strcmp(argv[i], "--junit")) junit = argv[++i];
        else if (!strcmp(argv[i], "--json")) json = argv[++i];
    }
    struct Result { std::string name, reqs; std::vector<std::string> fails; };
    std::vector<Result> results;
    int failed = 0;
    for (auto &c : registry()) {
        failures().clear();
        c.fn();
        results.push_back({c.name, c.reqs, failures()});
        bool ok = failures().empty();
        if (!ok) failed++;
        printf("[%s] %s.%s (%s)\n", ok ? " PASS " : " FAIL ", suite.c_str(), c.name, c.reqs);
        for (auto &f : failures()) printf("        %s\n", f.c_str());
    }
    printf("%s: %zu tests, %d failed\n", suite.c_str(), results.size(), failed);
    if (!junit.empty()) {
        FILE *f = fopen(junit.c_str(), "w");
        if (f) {
            fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"%s\" tests=\"%zu\" failures=\"%d\">\n",
                    xml(suite).c_str(), results.size(), failed);
            for (auto &r : results) {
                fprintf(f, "  <testcase classname=\"fw_host.%s\" name=\"%s [%s]\">\n", xml(suite).c_str(), xml(r.name).c_str(),
                        xml(r.reqs).c_str());
                fprintf(f, "    <properties><property name=\"requirements\" value=\"%s\"/></properties>\n", xml(r.reqs).c_str());
                for (auto &m : r.fails) fprintf(f, "    <failure message=\"%s\"/>\n", xml(m).c_str());
                fprintf(f, "  </testcase>\n");
            }
            fprintf(f, "</testsuite>\n");
            fclose(f);
        }
    }
    if (!json.empty()) {
        FILE *f = fopen(json.c_str(), "w");
        if (f) {
            fprintf(f, "[\n");
            for (size_t i = 0; i < results.size(); i++) {
                auto &r = results[i];
                fprintf(f, "  {\"layer\": \"fw_host\", \"suite\": \"%s\", \"test\": \"%s\", \"reqs\": \"%s\", \"outcome\": \"%s\"}%s\n",
                        suite.c_str(), r.name.c_str(), r.reqs.c_str(), r.fails.empty() ? "passed" : "failed",
                        i + 1 < results.size() ? "," : "");
            }
            fprintf(f, "]\n");
            fclose(f);
        }
    }
    return failed ? 1 : 0;
}

}  // namespace minitest

#define TEST(name, reqs)                                                   \
    static void name();                                                    \
    static minitest::Registrar name##_registrar(#name, reqs, name);        \
    static void name()

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) minitest::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        auto _va = (a);                                                                            \
        auto _vb = (b);                                                                            \
        if (!(_va == _vb))                                                                         \
            minitest::fail(__FILE__, __LINE__,                                                     \
                           "CHECK_EQ(" #a ", " #b "): " + minitest::show(_va) + " != " + minitest::show(_vb)); \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                      \
    do {                                                                                           \
        double _va = (a), _vb = (b);                                                               \
        if (!(_va - _vb <= (tol) && _vb - _va <= (tol)))                                           \
            minitest::fail(__FILE__, __LINE__,                                                     \
                           "CHECK_NEAR(" #a ", " #b "): " + minitest::show(_va) + " vs " + minitest::show(_vb)); \
    } while (0)

#define CHECK_CONTAINS(hay, needle)                                                                \
    do {                                                                                           \
        std::string _h = std::string((hay).c_str()), _n = (needle);                                \
        if (_h.find(_n) == std::string::npos)                                                      \
            minitest::fail(__FILE__, __LINE__, "CHECK_CONTAINS(" #hay "): \"" + _h + "\" lacks \"" + _n + "\""); \
    } while (0)

#define MINITEST_MAIN                                                      \
    int main(int argc, char **argv) { return minitest::run(argc, argv); }
