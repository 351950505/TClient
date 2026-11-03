#include <game/client/components/tclient/console_log_filter.h>

#include <gtest/gtest.h>

TEST(TcConsoleLogFilter, ClassifiesBySystemName)
{
	EXPECT_EQ(TcClassifyConsoleLogSystem("chat/all"), TC_CONSOLE_LOG_CATEGORY_PLAYER);
	EXPECT_EQ(TcClassifyConsoleLogSystem("chat/team"), TC_CONSOLE_LOG_CATEGORY_PLAYER);
	EXPECT_EQ(TcClassifyConsoleLogSystem("chat/whisper"), TC_CONSOLE_LOG_CATEGORY_PLAYER);

	EXPECT_EQ(TcClassifyConsoleLogSystem("binds"), TC_CONSOLE_LOG_CATEGORY_BINDS);
	EXPECT_EQ(TcClassifyConsoleLogSystem("binds/foo"), TC_CONSOLE_LOG_CATEGORY_BINDS);
	EXPECT_EQ(TcClassifyConsoleLogSystem("bindchat"), TC_CONSOLE_LOG_CATEGORY_BINDS);

	EXPECT_EQ(TcClassifyConsoleLogSystem("console"), TC_CONSOLE_LOG_CATEGORY_COMMAND);
	EXPECT_EQ(TcClassifyConsoleLogSystem("chatresp"), TC_CONSOLE_LOG_CATEGORY_COMMAND);
	EXPECT_EQ(TcClassifyConsoleLogSystem("chat/server"), TC_CONSOLE_LOG_CATEGORY_COMMAND);
	EXPECT_EQ(TcClassifyConsoleLogSystem("chat/client"), TC_CONSOLE_LOG_CATEGORY_COMMAND);

	EXPECT_EQ(TcClassifyConsoleLogSystem("client"), TC_CONSOLE_LOG_CATEGORY_SYSTEM);
	EXPECT_EQ(TcClassifyConsoleLogSystem("http"), TC_CONSOLE_LOG_CATEGORY_SYSTEM);
	EXPECT_EQ(TcClassifyConsoleLogSystem("skins"), TC_CONSOLE_LOG_CATEGORY_SYSTEM);
	EXPECT_EQ(TcClassifyConsoleLogSystem(""), TC_CONSOLE_LOG_CATEGORY_SYSTEM);
	EXPECT_EQ(TcClassifyConsoleLogSystem(nullptr), TC_CONSOLE_LOG_CATEGORY_SYSTEM);
}

TEST(TcConsoleLogFilter, PrefixBoundariesDoNotSwallowUnrelatedSystems)
{
	EXPECT_NE(TcClassifyConsoleLogSystem("bindsomething"), TC_CONSOLE_LOG_CATEGORY_BINDS);
	EXPECT_NE(TcClassifyConsoleLogSystem("chat/allx"), TC_CONSOLE_LOG_CATEGORY_PLAYER);
	EXPECT_NE(TcClassifyConsoleLogSystem("console2"), TC_CONSOLE_LOG_CATEGORY_COMMAND);
	// 大小写不敏感
	EXPECT_EQ(TcClassifyConsoleLogSystem("Binds"), TC_CONSOLE_LOG_CATEGORY_BINDS);
}

TEST(TcConsoleLogFilter, MaskNormalization)
{
	EXPECT_EQ(TcNormalizeConsoleLogFilterMask(0), TC_CONSOLE_LOG_CATEGORY_ALL);
	EXPECT_EQ(TcNormalizeConsoleLogFilterMask(0xFF), TC_CONSOLE_LOG_CATEGORY_ALL);
	EXPECT_EQ(TcNormalizeConsoleLogFilterMask(TC_CONSOLE_LOG_CATEGORY_PLAYER), TC_CONSOLE_LOG_CATEGORY_PLAYER);
}

TEST(TcConsoleLogFilter, SelectCategoryIsMonoSelect)
{
	// 点类别 = 只看该类别
	EXPECT_EQ(TcConsoleLogFilterSelectCategory(TC_CONSOLE_LOG_CATEGORY_ALL, TC_CONSOLE_LOG_CATEGORY_PLAYER), TC_CONSOLE_LOG_CATEGORY_PLAYER);
	EXPECT_EQ(TcConsoleLogFilterSelectCategory(2, TC_CONSOLE_LOG_CATEGORY_COMMAND), TC_CONSOLE_LOG_CATEGORY_COMMAND);
	// 再点同一类别 = 回到全部
	EXPECT_EQ(TcConsoleLogFilterSelectCategory(TC_CONSOLE_LOG_CATEGORY_PLAYER, TC_CONSOLE_LOG_CATEGORY_PLAYER), TC_CONSOLE_LOG_CATEGORY_ALL);
	// 点「全部」= 恢复全显
	EXPECT_EQ(TcConsoleLogFilterSelectCategory(TC_CONSOLE_LOG_CATEGORY_PLAYER, TC_CONSOLE_LOG_CATEGORY_ALL), TC_CONSOLE_LOG_CATEGORY_ALL);
	// 切到别的类别
	EXPECT_EQ(TcConsoleLogFilterSelectCategory(TC_CONSOLE_LOG_CATEGORY_PLAYER, TC_CONSOLE_LOG_CATEGORY_SYSTEM), TC_CONSOLE_LOG_CATEGORY_SYSTEM);
}

TEST(TcConsoleLogFilter, ButtonStatesAndFiltering)
{
	const int Mask = TC_CONSOLE_LOG_CATEGORY_ALL & ~TC_CONSOLE_LOG_CATEGORY_SYSTEM;
	EXPECT_TRUE(TcConsoleLogFilterButtonActive(Mask, TC_CONSOLE_LOG_CATEGORY_PLAYER));
	EXPECT_TRUE(TcConsoleLogFilterButtonActive(Mask, TC_CONSOLE_LOG_CATEGORY_COMMAND));
	EXPECT_TRUE(TcConsoleLogFilterButtonActive(Mask, TC_CONSOLE_LOG_CATEGORY_BINDS));
	EXPECT_FALSE(TcConsoleLogFilterButtonActive(Mask, TC_CONSOLE_LOG_CATEGORY_SYSTEM));
	EXPECT_FALSE(TcConsoleLogFilterButtonActive(Mask, TC_CONSOLE_LOG_CATEGORY_ALL));

	EXPECT_TRUE(TcConsoleLogCategoryPassesFilter(TC_CONSOLE_LOG_CATEGORY_PLAYER, Mask));
	EXPECT_FALSE(TcConsoleLogCategoryPassesFilter(TC_CONSOLE_LOG_CATEGORY_SYSTEM, Mask));
	// 非法类别不显示
	EXPECT_FALSE(TcConsoleLogCategoryPassesFilter(0, TC_CONSOLE_LOG_CATEGORY_ALL));
}

TEST(TcConsoleLogFilter, ButtonOrderMatchesQmClientLayout)
{
	EXPECT_EQ(TcConsoleLogFilterButtonCategory(0), TC_CONSOLE_LOG_CATEGORY_ALL);
	EXPECT_EQ(TcConsoleLogFilterButtonCategory(1), TC_CONSOLE_LOG_CATEGORY_PLAYER);
	EXPECT_EQ(TcConsoleLogFilterButtonCategory(2), TC_CONSOLE_LOG_CATEGORY_SYSTEM);
	EXPECT_EQ(TcConsoleLogFilterButtonCategory(3), TC_CONSOLE_LOG_CATEGORY_COMMAND);
	EXPECT_EQ(TcConsoleLogFilterButtonCategory(4), TC_CONSOLE_LOG_CATEGORY_BINDS);
}
