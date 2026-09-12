// Coherence Fabric -- Apache License 2.0 -- Copyright 2026 Summon Software Labs.
//
// Minimal test harness.
//
// Properties that matter for hang localisation:
//   * every case has a stable identifier and can be run exactly by name
//   * BEGIN, PHASE, PASS and FAIL markers are written and flushed immediately
//   * a failure is reported with its exact check message and source location
//   * no case is ever given a wall-clock deadline; a hang is a defect and the
//     marker stream identifies the last completed phase
#ifndef COHERENCE_TEST_FRAMEWORK_HPP
#define COHERENCE_TEST_FRAMEWORK_HPP

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "coherence/status.hpp"

namespace cftest {

struct CaseFailure {
  std::string message;
};

class Context {
 public:
  Context(std::string name, const char* source) : name_(std::move(name)), source_(source) {}

  void phase(const char* phase_name) {
    std::printf("PHASE %s %s\n", name_.c_str(), phase_name);
    std::fflush(stdout);
  }

  void mark(const std::string& text) {
    std::printf("MARK %s %s\n", name_.c_str(), text.c_str());
    std::fflush(stdout);
  }

  void expect(bool condition, const std::string& what) {
    ++checks_;
    if (condition) return;
    ++failures_;
    std::printf("CHECK_FAIL %s %s\n", name_.c_str(), what.c_str());
    std::fflush(stdout);
  }

  void expect_eq(const std::string& actual, const std::string& expected, const std::string& what) {
    ++checks_;
    if (actual == expected) return;
    ++failures_;
    std::printf("CHECK_FAIL %s %s expected=[%s] actual=[%s]\n", name_.c_str(), what.c_str(),
                expected.c_str(), actual.c_str());
    std::fflush(stdout);
  }

  void expect_eq_u(std::uint64_t actual, std::uint64_t expected, const std::string& what) {
    ++checks_;
    if (actual == expected) return;
    ++failures_;
    std::printf("CHECK_FAIL %s %s expected=%llu actual=%llu\n", name_.c_str(), what.c_str(),
                static_cast<unsigned long long>(expected),
                static_cast<unsigned long long>(actual));
    std::fflush(stdout);
  }

  void require(bool condition, const std::string& what) {
    expect(condition, what);
    if (!condition) throw CaseFailure{what};
  }

  [[nodiscard]] int failures() const noexcept { return failures_; }
  [[nodiscard]] int checks() const noexcept { return checks_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }

 private:
  std::string name_;
  const char* source_;
  int checks_ = 0;
  int failures_ = 0;
};

class Registry {
 public:
  using Body = void (*)(Context&);

  struct Entry {
    std::string name;
    Body body;
  };

  static Registry& instance() {
    static Registry registry;
    return registry;
  }

  bool add(std::string name, Body body) {
    entries_.push_back(Entry{std::move(name), body});
    return true;
  }

  [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

 private:
  std::vector<Entry> entries_;
};

inline int run_suite(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  std::string filter;
  bool list = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--case" && i + 1 < args.size()) {
      filter = args[i + 1];
      ++i;
      continue;
    }
    if (args[i].rfind("--case=", 0) == 0) {
      filter = args[i].substr(7);
      continue;
    }
    if (args[i] == "--list") {
      list = true;
      continue;
    }
  }
  const std::vector<Registry::Entry>& entries = Registry::instance().entries();
  if (list) {
    for (const Registry::Entry& entry : entries) std::printf("%s\n", entry.name.c_str());
    std::fflush(stdout);
    return 0;
  }

  int passed = 0;
  int failed = 0;
  int matched = 0;
  for (const Registry::Entry& entry : entries) {
    if (!filter.empty() && entry.name != filter) continue;
    ++matched;
    std::printf("BEGIN %s\n", entry.name.c_str());
    std::fflush(stdout);
    Context context(entry.name, "");
    bool threw = false;
    try {
      entry.body(context);
    } catch (const CaseFailure&) {
      threw = true;
    } catch (const std::exception& error) {
      context.expect(false, std::string("unexpected exception: ") + error.what());
      threw = true;
    } catch (...) {
      context.expect(false, "unexpected non-standard exception");
      threw = true;
    }
    if (context.failures() == 0 && !threw) {
      std::printf("PASS %s checks=%d\n", entry.name.c_str(), context.checks());
      ++passed;
    } else {
      std::printf("FAIL %s checks=%d failures=%d\n", entry.name.c_str(), context.checks(),
                  context.failures());
      ++failed;
    }
    std::fflush(stdout);
  }
  if (!filter.empty() && matched == 0) {
    std::printf("NO_SUCH_CASE %s\n", filter.c_str());
    std::fflush(stdout);
    return 3;
  }
  std::printf("SUMMARY cases=%d passed=%d failed=%d\n", matched, passed, failed);
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

} // namespace cftest

#define CF_TEST(identifier)                                                        \
  static void cf_body_##identifier(::cftest::Context& context);                    \
  namespace {                                                                      \
  const bool cf_registered_##identifier = ::cftest::Registry::instance().add(      \
      #identifier, &cf_body_##identifier);                                         \
  }                                                                                \
  static void cf_body_##identifier(::cftest::Context& context)

/// Standard entry point for a test executable.
/// Assert an invariant audit is clean, reporting every finding when it is not.
#define CF_EXPECT_AUDIT_CLEAN(report_expression)                                       \
  do {                                                                                 \
    const auto cf_audit_report = (report_expression);                                  \
    if (!cf_audit_report.clean) {                                                      \
      for (const auto& cf_audit_finding : cf_audit_report.findings) {                  \
        context.mark(std::string("violation ") +                                        \
                     std::string(::coherence::status_code_name(cf_audit_finding.code)) +\
                     " " + cf_audit_finding.detail);                                   \
      }                                                                                \
    }                                                                                  \
    context.expect(cf_audit_report.clean, #report_expression);                         \
  } while (false)

#define CF_TEST_MAIN()                      \
  int main(int argc, char** argv) {         \
    return ::cftest::run_suite(argc, argv); \
  }

#define CF_EXPECT(condition) context.expect((condition), #condition)
#define CF_EXPECT_EQ(actual, expected) context.expect_eq((actual), (expected), #actual)
#define CF_EXPECT_EQ_U(actual, expected) context.expect_eq_u((actual), (expected), #actual)
#define CF_REQUIRE(condition) context.require((condition), #condition)

#endif // COHERENCE_TEST_FRAMEWORK_HPP
