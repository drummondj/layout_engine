#include "../database.hpp"
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace le;

TEST(CompactVector, IsOnePointerAndEmptyAllocatesNothing)
{
    static_assert(sizeof(CompactVector<Rect>) == sizeof(void *));
    const CompactVector<Rect> empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.capacity(), 0u);
    EXPECT_EQ(empty.data(), nullptr);
    EXPECT_EQ(empty.begin(), empty.end());
}

TEST(CompactVector, GrowsKeepsOrderAndAlignsItsElements)
{
    CompactVector<Rect> rects;
    for (int64_t i = 0; i < 100; ++i)
        rects.push_back(Rect{{i, i}, {i + 1, i + 1}});
    ASSERT_EQ(rects.size(), 100u);
    EXPECT_GE(rects.capacity(), 100u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(rects.data()) % alignof(Rect), 0u);
    for (int64_t i = 0; i < 100; ++i)
        EXPECT_EQ(rects[static_cast<size_t>(i)].ll.x, i);
    EXPECT_EQ(rects.back().ur.x, 100);

    rects.shrink_to_fit();
    EXPECT_EQ(rects.capacity(), 100u);
    rects.clear();
    rects.shrink_to_fit();
    EXPECT_EQ(rects.capacity(), 0u);
}

TEST(CompactVector, CopiesMovesAndConvertsLikeAVector)
{
    const std::vector<std::string> source{"a", "b", "c"};
    CompactVector<std::string> list = source;
    EXPECT_TRUE(list == source);

    CompactVector<std::string> copy = list;
    copy[0] = "z";
    EXPECT_EQ(list[0], "a"); // a deep copy

    CompactVector<std::string> moved = std::move(copy);
    EXPECT_TRUE(copy.empty());
    EXPECT_EQ(moved.size(), 3u);
    EXPECT_EQ(moved.to_vector(), (std::vector<std::string>{"z", "b", "c"}));

    list = {"x"};
    EXPECT_EQ(list.size(), 1u);
    const std::span<const std::string> view = moved;
    EXPECT_EQ(view.size(), 3u);
}

TEST(CompactVector, InsertsAndErasesInPlace)
{
    CompactVector<int> list{1, 2, 4};
    list.insert(list.begin() + 2, 3);
    list.insert(list.begin(), 0);
    const std::vector<int> tail{5, 6};
    list.insert(list.end(), tail.begin(), tail.end());
    EXPECT_EQ(list.to_vector(), (std::vector<int>{0, 1, 2, 3, 4, 5, 6}));

    list.erase(list.begin() + 1);
    list.erase(list.begin() + 3, list.end() - 1);
    EXPECT_EQ(list.to_vector(), (std::vector<int>{0, 2, 3, 6}));

    list.resize(6, 9);
    EXPECT_EQ(list.to_vector(), (std::vector<int>{0, 2, 3, 6, 9, 9}));
    list.resize(2);
    EXPECT_EQ(list.to_vector(), (std::vector<int>{0, 2}));
}

TEST(CompactVector, DestroysWhatItHolds)
{
    const auto counted = std::make_shared<int>(0);
    {
        CompactVector<std::shared_ptr<int>> list;
        for (int i = 0; i < 10; ++i)
            list.push_back(counted);
        EXPECT_EQ(counted.use_count(), 11);
        list.erase(list.begin(), list.begin() + 4);
        EXPECT_EQ(counted.use_count(), 7);
    }
    EXPECT_EQ(counted.use_count(), 1);
}

TEST(CompactVector, ShapeDataStoresItsListsCompactly)
{
    static_assert(sizeof(ShapeData{}.rects) == sizeof(void *));
    ShapeData shape{.rects = {Rect{{0, 0}, {1, 1}}}};
    EXPECT_EQ(shape.rects.size(), 1u);
    EXPECT_TRUE(shape.texts.empty());
}
