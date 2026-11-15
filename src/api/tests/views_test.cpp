#include "views.hpp"
#include "api.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace std::chrono_literals;

    // Whether `future` finishes within a couple of seconds - long enough
    // for a loaded machine, short enough that a hang fails the test.
    template <class T>
    bool finishes(std::future<T> &future)
    {
        return future.wait_for(2s) == std::future_status::ready;
    }

    // Whether `future` is still waiting after a short while.
    template <class T>
    bool still_waiting(std::future<T> &future)
    {
        return future.wait_for(100ms) == std::future_status::timeout;
    }

    le::LibraryId add_library(le::Session &session, const std::string &name)
    {
        return session.write().create_library({.name = name}).value();
    }
}

TEST(Session, StartsEmpty)
{
    le::Session session;
    ASSERT_NE(session.handle(), nullptr);
    const le::ReadView view = session.read();
    ASSERT_TRUE(view.valid());
    EXPECT_TRUE(view.root().get_library_ids().empty());
    EXPECT_FALSE(view.current_technology().valid());
    EXPECT_FALSE(view.current_layout().valid());
    EXPECT_TRUE(view.selection().empty());
}

TEST(Session, AWriteIsSeenByALaterRead)
{
    le::Session session;
    const le::LibraryId library = add_library(session, "lib");
    const le::ReadView view = session.read();
    ASSERT_NE(view.root().get_library(library), nullptr);
    EXPECT_EQ(view.root().get_library(library)->name, "lib");
}

TEST(Session, MovingTransfersTheHandle)
{
    le::Session first;
    add_library(first, "lib");
    LeHandle *const handle = first.handle();

    le::Session second(std::move(first));
    EXPECT_EQ(first.handle(), nullptr);
    EXPECT_EQ(second.handle(), handle);
    EXPECT_EQ(second.read().root().get_library_ids().size(), 1u);

    le::Session third;
    third = std::move(second);
    EXPECT_EQ(second.handle(), nullptr);
    EXPECT_EQ(third.handle(), handle);
}

TEST(Session, AReleasedHandleOutlivesTheSession)
{
    LeHandle *handle = nullptr;
    {
        le::Session session;
        add_library(session, "lib");
        handle = session.release();
        EXPECT_EQ(session.handle(), nullptr);
    }
    EXPECT_EQ(le::ReadView(handle).root().get_library_ids().size(), 1u);
    le::Session::destroy(handle);
    le::Session::destroy(nullptr);
}

TEST(Session, ANewSessionHasNothingUnsaved)
{
    le::Session session;
    EXPECT_EQ(le_has_unsaved_database_changes(session.handle()), 0);
    EXPECT_EQ(le_has_unsaved_settings(session.handle()), 0);
}

TEST(Session, TryReadFailsWhileAnotherThreadWrites)
{
    le::Session session;
    std::promise<void> writing;
    std::promise<void> done;
    std::thread writer([&]
                       {
                           le::WriteView view = session.write();
                           writing.set_value();
                           done.get_future().wait(); });
    writing.get_future().wait();
    EXPECT_FALSE(session.try_read().valid());
    done.set_value();
    writer.join();
    EXPECT_TRUE(session.try_read().valid());
}

TEST(Transaction, GroupsEveryEditIntoOneUndoStep)
{
    le::Session session;
    {
        le::Transaction transaction = session.transaction("add two");
        add_library(session, "a");
        add_library(session, "b");
    }
    EXPECT_EQ(session.read().root().get_library_ids().size(), 2u);
    EXPECT_EQ(le_undo(session.handle()), 1);
    EXPECT_TRUE(session.read().root().get_library_ids().empty());
    EXPECT_EQ(le_can_undo(session.handle()), 0);
}

TEST(Transaction, InsideAnOpenStepDoesNothing)
{
    le::Session session;
    {
        le::Transaction outer = session.transaction("outer");
        {
            le::Transaction inner = session.transaction("inner");
            add_library(session, "a");
        }
        EXPECT_TRUE(session.renderer().renders_held());
        add_library(session, "b");
    }
    EXPECT_FALSE(session.renderer().renders_held());
    EXPECT_EQ(le_undo(session.handle()), 1);
    EXPECT_TRUE(session.read().root().get_library_ids().empty());
}

TEST(Transaction, AFailedOneIsStillUndoable)
{
    le::Session session;
    {
        le::Transaction transaction = session.transaction("half done");
        add_library(session, "a");
        transaction.fail();
    }
    EXPECT_EQ(le_undo(session.handle()), 1);
    EXPECT_TRUE(session.read().root().get_library_ids().empty());
}

TEST(Renderer, AnUnsizedViewportRendersAnEmptyFrame)
{
    le::Session session;
    const le::RenderedFrame frame = session.renderer().render();
    EXPECT_TRUE(frame.empty());
    EXPECT_EQ(frame.width, 0);
    EXPECT_EQ(frame.height, 0);
}

TEST(Renderer, RendersAtTheViewportSize)
{
    le::Session session;
    le_set_viewport_size(session.handle(), 64, 32);
    const le::RenderedFrame frame = session.renderer().render();
    ASSERT_FALSE(frame.empty());
    EXPECT_EQ(frame.width, 64);
    EXPECT_EQ(frame.height, 32);
    EXPECT_GE(frame.row_bytes, 64u * 4u);
    EXPECT_FALSE(session.renderer().is_rendering());
}

TEST(Renderer, AFrameOutlivesLaterRenders)
{
    le::Session session;
    le_set_viewport_size(session.handle(), 40, 20);
    const le::RenderedFrame first = session.renderer().render();
    ASSERT_FALSE(first.empty());
    const std::vector<uint8_t> pixels(first.data, first.data + first.row_bytes * 20);

    le_set_viewport_size(session.handle(), 80, 60);
    const le::RenderedFrame second = session.renderer().render();
    ASSERT_FALSE(second.empty());
    EXPECT_EQ(second.width, 80);
    EXPECT_NE(second.data, first.data);

    EXPECT_EQ(first.width, 40);
    EXPECT_EQ(first.height, 20);
    EXPECT_EQ(std::vector<uint8_t>(first.data, first.data + first.row_bytes * 20), pixels);
}

TEST(Renderer, AWriteWakesTheWait)
{
    le::Session session;
    const le::Renderer renderer = session.renderer();
    // A new session has never been rendered, so the first wait may return
    // at once; drain it.
    session.write();
    renderer.wait_until_needed();

    std::future<void> wait = std::async(std::launch::async, [&]
                                        { renderer.wait_until_needed(); });
    EXPECT_TRUE(still_waiting(wait));
    add_library(session, "lib");
    EXPECT_TRUE(finishes(wait));
}

TEST(Renderer, CancelWakesTheWaitWithoutAWrite)
{
    le::Session session;
    const le::Renderer renderer = session.renderer();
    session.write();
    renderer.wait_until_needed();

    std::future<void> wait = std::async(std::launch::async, [&]
                                        { renderer.wait_until_needed(); });
    EXPECT_TRUE(still_waiting(wait));
    renderer.cancel_wait();
    EXPECT_TRUE(finishes(wait));
}

TEST(Renderer, ACancelBeforeTheWaitIsKept)
{
    le::Session session;
    const le::Renderer renderer = session.renderer();
    session.write();
    renderer.wait_until_needed();

    renderer.cancel_wait();
    std::future<void> wait = std::async(std::launch::async, [&]
                                        { renderer.wait_until_needed(); });
    EXPECT_TRUE(finishes(wait));
}

TEST(Renderer, ATransactionDefersTheWakeUntilItEnds)
{
    le::Session session;
    const le::Renderer renderer = session.renderer();
    session.write();
    renderer.wait_until_needed();

    std::future<void> wait;
    {
        le::Transaction transaction = session.transaction("held");
        EXPECT_TRUE(renderer.renders_held());
        wait = std::async(std::launch::async, [&]
                          { renderer.wait_until_needed(); });
        add_library(session, "lib");
        EXPECT_TRUE(still_waiting(wait));
    }
    EXPECT_FALSE(renderer.renders_held());
    EXPECT_TRUE(finishes(wait));
}
