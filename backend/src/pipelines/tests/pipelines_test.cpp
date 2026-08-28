// Phase 0 (backend/ONETBB_INTEGRATION.md migration plan) toolchain-proving
// test: exercises MemoizingStage's cache-hit/recompute contract directly,
// with no dependency on any real pipeline logic yet. Once real stages land
// under src/pipelines/stages/ (Phase 1+), this file's DoublingStage
// throwaway is expected to be replaced/joined by real stage tests - it
// exists purely to prove TBB + Tracy build/link before any domain code is
// written.
#include "../tbb_core.hpp"
#include <gtest/gtest.h>
#include <memory>

namespace
{
    struct TestOptions
    {
        int trigger = 0;
        bool operator==(const TestOptions &) const = default;
    };

    class DoublingStage : public le::MemoizingStage<int, int, TestOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;
        int compute_count = 0;

    protected:
        int compute(const int &data, const TestOptions &options) override
        {
            ++compute_count;
            return data * 2 + options.trigger;
        }

        bool options_did_change(const TestOptions &last, const TestOptions &current) const override
        {
            return last.trigger != current.trigger;
        }
    };
}

TEST(MemoizingStage, CacheHitSkipsRecomputeForUnchangedDataVersionAndOptions)
{
    using namespace oneapi::tbb::flow;
    graph g;
    auto stage = std::make_unique<DoublingStage>(g, "doubling");

    le::StageData<int, TestOptions> received{};
    function_node<le::StageData<int, TestOptions>> sink(g, serial, [&](le::StageData<int, TestOptions> in)
                                                          { received = in; });
    make_edge(stage->node(), sink);

    TestOptions options{.trigger = 1};

    stage->try_put({.data = 21, .data_version = 1, .options = options});
    g.wait_for_all();
    EXPECT_EQ(received.data, 43);
    EXPECT_EQ(stage->compute_count, 1);

    // Same data_version, same options -> cache hit, no recompute, but the
    // node still forwards the previous result downstream.
    stage->try_put({.data = 21, .data_version = 1, .options = options});
    g.wait_for_all();
    EXPECT_EQ(stage->compute_count, 1);
    EXPECT_EQ(received.data, 43);

    // data_version changed -> recompute, even though the payload's own
    // value happens to be unchanged (MemoizingStage never inspects data
    // itself, only data_version).
    stage->try_put({.data = 21, .data_version = 2, .options = options});
    g.wait_for_all();
    EXPECT_EQ(stage->compute_count, 2);

    // data_version unchanged, but options_did_change() says recompute.
    TestOptions changed_options{.trigger = 5};
    stage->try_put({.data = 21, .data_version = 2, .options = changed_options});
    g.wait_for_all();
    EXPECT_EQ(stage->compute_count, 3);
    EXPECT_EQ(received.data, 47);
}
