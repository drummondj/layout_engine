#include "hello_ext/hello.hpp"

#include <gtest/gtest.h>

namespace
{
    struct Session
    {
        LeHandle *handle = le_create();
        ~Session() { le_destroy(handle); }
    };
}

TEST(HelloExt, IsRegistered)
{
    le::ext::register_all();
    le::ext::register_all(); // idempotent
    int found = 0;
    for (int32_t i = 0; i < le_extension_count(); ++i)
        if (std::string(le_extension_name(i)) == "hello_ext")
        {
            ++found;
            EXPECT_STREQ(le_extension_version(i), "0.1.0");
        }
    EXPECT_EQ(found, 1);
    EXPECT_EQ(le_extension_name(le_extension_count()), nullptr);
}

TEST(HelloExt, ReadsTheDatabase)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    EXPECT_EQ(hello::library_count(ctx), 0);
    le_create_library(session.handle, "lib1");
    EXPECT_EQ(hello::library_count(ctx), 1);
}

TEST(HelloExt, AnEditIsOneUndoStep)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    ASSERT_TRUE(hello::add_greeting(ctx, "world"));
    EXPECT_EQ(hello::library_count(ctx), 1);
    ASSERT_NE(le_undo(session.handle), 0);
    EXPECT_EQ(hello::library_count(ctx), 0);
}

TEST(HelloExt, StateIsPerSessionAndPerExtension)
{
    Session a, b;
    le::ext::ExtensionContext in_a(a.handle, "hello_ext");
    le::ext::ExtensionContext in_b(b.handle, "hello_ext");
    le::ext::ExtensionContext other_extension(a.handle, "other_ext");
    ASSERT_TRUE(hello::add_greeting(in_a, "one"));
    ASSERT_TRUE(hello::add_greeting(in_a, "two"));
    EXPECT_EQ(in_a.data<hello::State>().greetings_added, 2);
    EXPECT_EQ(in_b.data<hello::State>().greetings_added, 0);
    EXPECT_EQ(other_extension.data<hello::State>().greetings_added, 0);
}

TEST(HelloExt, AFailedEditLeavesNothingToUndo)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    EXPECT_FALSE(hello::add_greeting(ctx, ""));
    EXPECT_EQ(ctx.data<hello::State>().greetings_added, 0);
    EXPECT_EQ(le_undo(session.handle), 0);
}

TEST(HelloExt, AWriteViewEditIsSeenByReaders)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    {
        le::ext::WriteView view = ctx.write();
        view.root().create_library(le::LibraryData{.name = "bulk"});
    }
    EXPECT_EQ(hello::library_count(ctx), 1);
}
