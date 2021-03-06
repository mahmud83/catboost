#include "iterator.h"

#include <library/unittest/registar.h>

template <typename I, typename C>
void TestStringSplitterCount(I* str, C delim, size_t good) {
    size_t res = StringSplitter(str).Split(delim).Count();
    UNIT_ASSERT_VALUES_EQUAL(res, good);
}

Y_UNIT_TEST_SUITE(StringSplitter) {
    Y_UNIT_TEST(TestSplit) {
        int sum = 0;

        for (const auto& it : StringSplitter("1,2,3").Split(',')) {
            sum += FromString<int>(it.Token());
        }

        UNIT_ASSERT_VALUES_EQUAL(sum, 6);
    }

    Y_UNIT_TEST(TestSplit1) {
        int cnt = 0;

        for (const auto& it : StringSplitter(" ").Split(' ')) {
            (void)it;

            ++cnt;
        }

        UNIT_ASSERT_VALUES_EQUAL(cnt, 2);
    }

    Y_UNIT_TEST(TestSplitLimited) {
        TVector<TString> expected = {"1", "2", "3,4,5"};
        TVector<TString> actual = StringSplitter("1,2,3,4,5").SplitLimited(',', 3).ToList<TString>();
        UNIT_ASSERT_VALUES_EQUAL(expected, actual);
    }

    Y_UNIT_TEST(TestSplitBySet) {
        int sum = 0;

        for (const auto& it : StringSplitter("1,2:3").SplitBySet(",:")) {
            sum += FromString<int>(it.Token());
        }

        UNIT_ASSERT_VALUES_EQUAL(sum, 6);
    }

    Y_UNIT_TEST(TestSplitBySetLimited) {
        TVector<TString> expected = {"1", "2", "3,4:5"};
        TVector<TString> actual = StringSplitter("1,2:3,4:5").SplitBySetLimited(",:", 3).ToList<TString>();
        UNIT_ASSERT_VALUES_EQUAL(expected, actual);
    }

    Y_UNIT_TEST(TestSplitByString) {
        int sum = 0;

        for (const auto& it : StringSplitter("1ab2ab3").SplitByString("ab")) {
            sum += FromString<int>(it.Token());
        }

        UNIT_ASSERT_VALUES_EQUAL(sum, 6);
    }

    Y_UNIT_TEST(TestSplitByStringLimited) {
        TVector<TString> expected = {"1", "2", "3ab4ab5"};
        TVector<TString> actual = StringSplitter("1ab2ab3ab4ab5").SplitByStringLimited("ab", 3).ToList<TString>();
        UNIT_ASSERT_VALUES_EQUAL(expected, actual);
    }

    Y_UNIT_TEST(TestSplitByFunc) {
        TString s = "123 456 \t\n789\n10\t 20";
        TVector<TString> pattern = {"123", "456", "789", "10", "20"};

        TVector<TString> tokens;
        auto f = [](char a) { return a == ' ' || a == '\t' || a == '\n'; };
        for (auto v : StringSplitter(s).SplitByFunc(f)) {
            if (v.Empty() == false) {
                tokens.emplace_back(v.TokenStart(), v.TokenDelim());
            }
        }

        UNIT_ASSERT(tokens == pattern);
    }

    Y_UNIT_TEST(TestSplitByFuncLimited) {
        TVector<TString> expected = {"1", "2", "3a4b5"};
        auto f = [](char a) { return a == 'a' || a == 'b'; };
        TVector<TString> actual = StringSplitter("1a2b3a4b5").SplitByFuncLimited(f, 3).ToList<TString>();
        UNIT_ASSERT_VALUES_EQUAL(expected, actual);
    }

    Y_UNIT_TEST(TestSkipEmpty) {
        int sum = 0;

        for (const auto& it : StringSplitter("  1 2 3   ").Split(' ').SkipEmpty()) {
            sum += FromString<int>(it.Token());
        }

        UNIT_ASSERT_VALUES_EQUAL(sum, 6);

        // double
        sum = 0;
        for (const auto& it : StringSplitter("  1 2 3   ").Split(' ').SkipEmpty().SkipEmpty()) {
            sum += FromString<int>(it.Token());
        }
        UNIT_ASSERT_VALUES_EQUAL(sum, 6);
    }

    Y_UNIT_TEST(TestTake) {
        TVector<TString> expected = {"1", "2", "3"};
        UNIT_ASSERT_VALUES_EQUAL(expected, StringSplitter("1 2 3 4 5 6 7 8 9 10").Split(' ').Take(3).ToList<TString>());

        expected = {"1", "2"};
        UNIT_ASSERT_VALUES_EQUAL(expected, StringSplitter("  1 2 3   ").Split(' ').SkipEmpty().Take(2).ToList<TString>());

        expected = {"1", "2", "3"};
        UNIT_ASSERT_VALUES_EQUAL(expected, StringSplitter("1 2 3 4 5 6 7 8 9 10").Split(' ').Take(5).Take(3).ToList<TString>());
        UNIT_ASSERT_VALUES_EQUAL(expected, StringSplitter("1 2 3 4 5 6 7 8 9 10").Split(' ').Take(3).Take(5).ToList<TString>());

        expected = {"1", "2"};
        UNIT_ASSERT_VALUES_EQUAL(expected, StringSplitter("  1 2 3  ").Split(' ').Take(4).SkipEmpty().ToList<TString>());

        expected = {"1"};
        UNIT_ASSERT_VALUES_EQUAL(expected, StringSplitter("  1 2 3  ").Split(' ').Take(4).SkipEmpty().Take(1).ToList<TString>());
    }

    Y_UNIT_TEST(TestCompileAbility) {
        (void)StringSplitter(TString());
        (void)StringSplitter(TStringBuf());
        (void)StringSplitter("", 0);
    }

    Y_UNIT_TEST(TestStringSplitterCountEmpty) {
        TCharDelimiter<const char> delim(' ');
        TestStringSplitterCount("", delim, 1);
    }

    Y_UNIT_TEST(TestStringSplitterCountOne) {
        TCharDelimiter<const char> delim(' ');
        TestStringSplitterCount("one", delim, 1);
    }

    Y_UNIT_TEST(TestStringSplitterCountWithOneDelimiter) {
        TCharDelimiter<const char> delim(' ');
        TestStringSplitterCount("one two", delim, 2);
    }

    Y_UNIT_TEST(TestStringSplitterCountWithTrailing) {
        TCharDelimiter<const char> delim(' ');
        TestStringSplitterCount(" one ", delim, 3);
    }

    Y_UNIT_TEST(TestStringSplitterConsume) {
        TVector<TString> expected = {"1", "2", "3"};
        TVector<TString> actual;
        auto func = [&actual](const TGenericStringBuf<char>& token) {
            actual.push_back(TString(token));
        };
        StringSplitter("1 2 3").Split(' ').Consume(func);
        UNIT_ASSERT_VALUES_EQUAL(expected, actual);
    }

    Y_UNIT_TEST(TestStringSplitterToList) {
        TVector<TString> expected = {"1", "2", "3"};
        TVector<TString> actual = StringSplitter("1 2 3").Split(' ').ToList<TString>();
        UNIT_ASSERT_VALUES_EQUAL(expected, actual);
    }

    Y_UNIT_TEST(TestStringSplitterCollectPushBack) {
        TVector<TString> expected = {"1", "2", "3"};
        TVector<TString> actual;
        StringSplitter("1 2 3").Split(' ').Collect(&actual);
        UNIT_ASSERT_VALUES_EQUAL(expected, actual);
    }

    Y_UNIT_TEST(TestStringSplitterCollectInsert) {
        TSet<TString> expected = {"1", "2", "3"};
        TSet<TString> actual;
        StringSplitter("1 2 3 1 2 3").Split(' ').Collect(&actual);
        UNIT_ASSERT_VALUES_EQUAL(expected, actual);
    }

    Y_UNIT_TEST(TestStringSplitterCollectClears) {
        TVector<TString> v;
        StringSplitter("1 2 3").Split(' ').Collect(&v);
        UNIT_ASSERT_VALUES_EQUAL(v.size(), 3);
        StringSplitter("4 5").Split(' ').Collect(&v);
        UNIT_ASSERT_VALUES_EQUAL(v.size(), 2);
    }

    Y_UNIT_TEST(TestStringSplitterAddToDoesntClear) {
        TVector<TString> v;
        StringSplitter("1 2 3").Split(' ').AddTo(&v);
        UNIT_ASSERT_VALUES_EQUAL(v.size(), 3);
        StringSplitter("4 5").Split(' ').AddTo(&v);
        UNIT_ASSERT_VALUES_EQUAL(v.size(), 5);
    }

    Y_UNIT_TEST(TestSplitStringInto) {
        int a = -1;
        TStringBuf s;
        double d = -1;
        StringSplitter("2 substr 1.02").Split(' ').CollectInto(&a, &s, &d);
        UNIT_ASSERT_VALUES_EQUAL(a, 2);
        UNIT_ASSERT_VALUES_EQUAL(s, "substr");
        UNIT_ASSERT_DOUBLES_EQUAL(d, 1.02, 0.0001);
        UNIT_ASSERT_EXCEPTION(StringSplitter("1").Split(' ').CollectInto(&a, &a), yexception);
        UNIT_ASSERT_EXCEPTION(StringSplitter("1 2 3").Split(' ').CollectInto(&a, &a), yexception);
    }

    Y_UNIT_TEST(TestOwningSplit1) {
        int sum = 0;

        for (const auto& it : StringSplitter(TString("1,2,3")).Split(',')) {
            sum += FromString<int>(it.Token());
        }

        UNIT_ASSERT_VALUES_EQUAL(sum, 6);
    }

    Y_UNIT_TEST(TestOwningSplit2) {
        int sum = 0;

        TString str("1,2,3");
        for (const auto& it : StringSplitter(str).Split(',')) {
            sum += FromString<int>(it.Token());
        }

        UNIT_ASSERT_VALUES_EQUAL(sum, 6);
    }

    Y_UNIT_TEST(TestOwningSplit3) {
        int sum = 0;

        const TString str("1,2,3");
        for (const auto& it : StringSplitter(str).Split(',')) {
            sum += FromString<int>(it.Token());
        }

        UNIT_ASSERT_VALUES_EQUAL(sum, 6);
    }

    Y_UNIT_TEST(TestAssigment) {
        TVector<TString> expected0 = { "1", "2", "3", "4" };
        TVector<TString> actual0 = StringSplitter("1 2 3 4").Split(' ');
        UNIT_ASSERT_VALUES_EQUAL(expected0, actual0);

        TSet<TString> expected1 = { "11", "22", "33", "44" };
        TSet<TString> actual1 = StringSplitter("11 22 33 44").Split(' ');
        UNIT_ASSERT_VALUES_EQUAL(expected1, actual1);

        TSet<TString> expected2 = { "11", "aa" };
        auto actual2 = static_cast<TSet<TString>>(StringSplitter("11 aa 11 11 aa").Split(' '));
        UNIT_ASSERT_VALUES_EQUAL(expected2, actual2);

        TVector<TString> expected3 = { "dd", "bb" };
        auto actual3 = TVector<TString>(StringSplitter("dd\tbb").Split('\t'));
        UNIT_ASSERT_VALUES_EQUAL(expected3, actual3);
    }
}
