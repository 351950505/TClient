#include <base/str.h>

#include <engine/shared/config.h>

#include <game/client/components/menus_checkbox_help.h>

#include <gtest/gtest.h>

TEST(ClientCheckBoxHelp, HasEntries)
{
	// ME 页约 24 个、TC 页各子页签约百来个真实开关都应有通俗说明；
	// 空表意味着悬停帮助功能没有真正接入。
	EXPECT_EQ(NumCheckBoxHelpEntries(), 141);
}

TEST(ClientCheckBoxHelp, AddressesAreUnique)
{
	const int Count = NumCheckBoxHelpEntries();
	for(int i = 0; i < Count; ++i)
	{
		for(int j = i + 1; j < Count; ++j)
		{
			EXPECT_NE(g_aCheckBoxHelp[i].m_pVariable, g_aCheckBoxHelp[j].m_pVariable)
				<< "duplicate entries " << i << " and " << j;
		}
	}
}

TEST(ClientCheckBoxHelp, AddressesPointIntoConfigObject)
{
	const char *pStart = reinterpret_cast<const char *>(&g_Config);
	const char *pEnd = pStart + sizeof(CConfig);
	const int Count = NumCheckBoxHelpEntries();
	for(int i = 0; i < Count; ++i)
	{
		const char *p = reinterpret_cast<const char *>(g_aCheckBoxHelp[i].m_pVariable);
		EXPECT_GE(p, pStart) << "entry " << i << " points outside g_Config";
		EXPECT_LT(p, pEnd) << "entry " << i << " points outside g_Config";
		EXPECT_EQ(reinterpret_cast<intptr_t>(p) % alignof(int), 0) << "entry " << i << " not int-aligned";
	}
}

TEST(ClientCheckBoxHelp, UnknownVariablesHaveNoHelp)
{
	// 普通 UI 复选框、临时状态变量不得命中帮助表
	int Local = 0;
	EXPECT_EQ(FindCheckBoxHelpText(&Local), nullptr);
	EXPECT_EQ(FindCheckBoxHelpText(nullptr), nullptr);
}

TEST(ClientCheckBoxHelp, HelpTextsArePresentable)
{
	const int Count = NumCheckBoxHelpEntries();
	for(int i = 0; i < Count; ++i)
	{
		const char *pHelp = g_aCheckBoxHelp[i].m_pHelp;
		ASSERT_NE(pHelp, nullptr) << "entry " << i;
		EXPECT_NE(pHelp[0], '\0') << "entry " << i << " has empty help";
		EXPECT_EQ(str_find_nocase(pHelp, "\n"), nullptr) << "entry " << i << " must stay on one line";
	}
}
