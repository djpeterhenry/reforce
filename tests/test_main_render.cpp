// doctest main for test executables that render: with REFORCE_GPU_POISON=1, checks the float textures each test
// case rendered into for NaN (render/GpuPoison.hpp) and fails the run when one holds any.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <string>

#include "render/GpuPoison.hpp"

namespace {

bool gPoisonFailed = false;

struct PoisonListener : doctest::IReporter {
    explicit PoisonListener(const doctest::ContextOptions&) {}
    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override { reforce::render::poison::startChecks(); }
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_start(const doctest::TestCaseData& tc) override { name_ = tc.m_name; }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {
        for (const auto& line : reforce::render::poison::check()) {
            std::fprintf(stderr, "GPU poison, test case \"%s\": %s\n", name_.c_str(), line.c_str());
            gPoisonFailed = true;
        }
    }
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}

private:
    std::string name_;
};

REGISTER_LISTENER("gpu-poison", 1, PoisonListener);

}  // namespace

int main(int argc, char** argv) {
    doctest::Context context(argc, argv);
    const int rc = context.run();
    if (context.shouldExit()) return rc;
    return rc != 0 ? rc : (gPoisonFailed ? 1 : 0);
}
