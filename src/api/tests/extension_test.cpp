#include "le/extension.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

namespace
{
    // Runs `f` on its own thread and returns what it returned, or nullopt if
    // it hasn't finished in a few seconds - so a deadlock fails the test
    // instead of hanging it. A thread that never finishes is left running.
    // An exception (libstdc++ throws EDEADLK for a same-thread re-lock)
    // is rethrown here, where gtest reports it.
    template <class F>
    auto within_timeout(F f) -> std::optional<decltype(f())>
    {
        auto result = std::make_shared<std::promise<decltype(f())>>();
        std::future<decltype(f())> future = result->get_future();
        std::thread([result, f]() {
            try
            {
                result->set_value(f());
            }
            catch (...)
            {
                result->set_exception(std::current_exception());
            }
        }).detach();
        if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            return std::nullopt;
        return future.get();
    }

    LeLayoutId add_layout(LeHandle *handle)
    {
        return le_create_layout(handle, le_create_design(handle, le_create_library(handle, "lib"), "top"));
    }
}

TEST(ExtensionIds, RoundTripBetweenTheCApiAndTheDatabase)
{
    const le::LayoutId id{.index = 7, .generation = 3};
    const LeLayoutId c = le::ext::to_c(id);
    EXPECT_EQ(c.index, 7u);
    EXPECT_EQ(c.generation, 3u);
    EXPECT_EQ(le::ext::from_c(c), id);
}

TEST(ExtensionIds, AnInvalidIdStaysInvalid)
{
    EXPECT_EQ(le::ext::to_c(le::DesignId{}).index, UINT32_MAX);
    EXPECT_FALSE(le::ext::from_c(LeDesignId{UINT32_MAX, 0}).valid());
}

TEST(ExtensionContext, CurrentObjectsAreInvalidUntilSet)
{
    LeHandle *handle = le_create();
    const le::ext::ExtensionContext ctx(handle, "test_ext");
    EXPECT_FALSE(ctx.current_technology().valid());
    EXPECT_FALSE(ctx.current_abstract().valid());
    EXPECT_FALSE(ctx.current_schematic().valid());
    EXPECT_FALSE(ctx.current_layout().valid());
    le_destroy(handle);
}

TEST(ExtensionContext, CurrentObjectsMatchWhatWasSet)
{
    LeHandle *handle = le_create();
    const LeLayoutId layout = add_layout(handle);
    const LeSchematicId schematic = le_create_schematic(handle, le_create_design(handle, le_create_library(handle, "logic"), "top"));
    ASSERT_EQ(le_set_current_layout(handle, layout), 0);
    ASSERT_EQ(le_set_current_schematic(handle, schematic), 0);

    const le::ext::ExtensionContext ctx(handle, "test_ext");
    EXPECT_EQ(ctx.current_layout(), le::ext::from_c(layout));
    EXPECT_EQ(ctx.current_schematic(), le::ext::from_c(schematic));
    le_destroy(handle);
}

TEST(ExtensionContext, CurrentObjectsAreReadableInsideViewsAndTransactions)
{
    LeHandle *handle = le_create();
    const LeLayoutId layout = add_layout(handle);
    ASSERT_EQ(le_set_current_layout(handle, layout), 0);
    const le::LayoutId expected = le::ext::from_c(layout);

    const auto in_read = within_timeout([handle]() {
        le::ext::ExtensionContext ctx(handle, "test_ext");
        const le::ext::ReadView view = ctx.read();
        return view.current_layout();
    });
    ASSERT_TRUE(in_read.has_value()) << "ReadView::current_layout deadlocked";
    EXPECT_EQ(*in_read, expected);

    const auto in_write = within_timeout([handle]() {
        le::ext::ExtensionContext ctx(handle, "test_ext");
        const le::ext::WriteView view = ctx.write();
        return view.current_layout();
    });
    ASSERT_TRUE(in_write.has_value()) << "WriteView::current_layout deadlocked";
    EXPECT_EQ(*in_write, expected);

    const auto in_transaction = within_timeout([handle]() {
        le::ext::ExtensionContext ctx(handle, "test_ext");
        const le::ext::Transaction transaction = ctx.transaction("test");
        return ctx.current_layout();
    });
    ASSERT_TRUE(in_transaction.has_value()) << "ExtensionContext::current_layout deadlocked in a transaction";
    EXPECT_EQ(*in_transaction, expected);

    le_destroy(handle);
}
