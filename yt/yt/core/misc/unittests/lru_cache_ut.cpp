#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/core/misc/sync_cache.h>

namespace NYT {
namespace {

////////////////////////////////////////////////////////////////////////////////

TEST(TSimpleLruCache, Common)
{
    TSimpleLruCache<TString, int> cache(2);
    cache.Insert("a", 1);
    cache.Insert("b", 2);

    EXPECT_TRUE(cache.Find("a"));
    EXPECT_TRUE(cache.Find("b"));
    EXPECT_FALSE(cache.Find("c"));
    EXPECT_EQ(cache.Get("a"), 1);
    EXPECT_EQ(cache.Get("b"), 2);

    cache.Insert("c", 3);

    EXPECT_FALSE(cache.Find("a"));
    EXPECT_TRUE(cache.Find("b"));
    EXPECT_TRUE(cache.Find("c"));
    EXPECT_EQ(cache.Get("b"), 2);
    EXPECT_EQ(cache.Get("c"), 3);

    cache.Insert("b", 4);

    EXPECT_FALSE(cache.Find("a"));
    EXPECT_TRUE(cache.Find("b"));
    EXPECT_TRUE(cache.Find("c"));
    EXPECT_EQ(cache.Get("c"), 3);
    EXPECT_EQ(cache.Get("b"), 4);

    cache.Insert("a", 5);

    EXPECT_TRUE(cache.Find("a"));
    EXPECT_TRUE(cache.Find("b"));
    EXPECT_FALSE(cache.Find("c"));
    EXPECT_EQ(cache.Get("a"), 5);
    EXPECT_EQ(cache.Get("b"), 4);
}

TEST(TSimpleLruCache, Clear)
{
    TSimpleLruCache<TString, int> cache(2);
    cache.Insert("a", 1);
    cache.Insert("b", 2);

    cache.Clear();
    EXPECT_FALSE(cache.Find("a"));
    EXPECT_FALSE(cache.Find("b"));

    cache.Insert("c", 3);
    cache.Insert("d", 4);
    cache.Insert("e", 5);

    EXPECT_FALSE(cache.Find("c"));
    EXPECT_TRUE(cache.Find("d"));
    EXPECT_TRUE(cache.Find("e"));
}

////////////////////////////////////////////////////////////////////////////////

TEST(TMultiLruCache, InsertAndFind)
{
    TMultiLruCache<TString, int> cache(3);

    EXPECT_EQ(cache.GetSize(), 0);

    cache.Insert("a", 1);
    cache.Insert("b", 2);
    cache.Insert("a", 3);

    EXPECT_EQ(cache.GetSize(), 3);

    ASSERT_TRUE(cache.Find("a"));
    ASSERT_TRUE(cache.Find("b"));
    EXPECT_FALSE(cache.Find("c"));

    EXPECT_EQ(cache.Get("a"), 3);
    EXPECT_EQ(cache.Get("a"), 1);
    EXPECT_EQ(cache.Get("b"), 2);
    EXPECT_EQ(cache.Get("b"), 2);

    cache.Insert("b", 4);

    EXPECT_EQ(cache.GetSize(), 3);
    ASSERT_TRUE(cache.Find("a"));
    ASSERT_TRUE(cache.Find("b"));
    EXPECT_FALSE(cache.Find("c"));

    EXPECT_EQ(cache.Get("a"), 1);
    EXPECT_EQ(cache.Get("a"), 1);
    EXPECT_EQ(cache.Get("b"), 4);
    EXPECT_EQ(cache.Get("b"), 2);

    EXPECT_TRUE(cache.Find("a"));

    cache.Insert("c", 5);

    EXPECT_EQ(cache.GetSize(), 3);
    EXPECT_TRUE(cache.Find("a"));
    EXPECT_TRUE(cache.Find("b"));
    EXPECT_TRUE(cache.Find("c"));

    cache.Clear();
    EXPECT_EQ(cache.GetSize(), 0);
}

TEST(TMultiLruCache, Extract)
{
    TMultiLruCache<TString, int> cache(3);

    cache.Insert("a", 1);
    cache.Insert("b", 2);
    cache.Insert("a", 3);

    EXPECT_FALSE(cache.TryExtract("c"));

    EXPECT_EQ(*cache.TryExtract("a"), 1);
    EXPECT_EQ(cache.GetSize(), 2);

    cache.Insert("b", 4);

    EXPECT_EQ(*cache.TryExtract("a"), 3);
    EXPECT_EQ(cache.GetSize(), 2);

    cache.Insert("c", 1);

    EXPECT_TRUE(cache.Find("b"));
    EXPECT_EQ(*cache.TryExtract("b"), 4);
    EXPECT_EQ(cache.GetSize(), 2);
    EXPECT_TRUE(cache.Find("b"));
    EXPECT_EQ(*cache.TryExtract("b"), 2);

    EXPECT_FALSE(cache.TryExtract("b"));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT
