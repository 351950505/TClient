#include <game/client/components/tclient/relay_client.h>

#include <gtest/gtest.h>

TEST(TClientRelay, DemoPlaybackNeverRegisters)
{
	// 原始崩溃路径：demo 的服务器地址类型为 0，不能传给 net_addr_str。
	EXPECT_FALSE(RelayRouting::CanRegister(IClient::STATE_DEMOPLAYBACK, NETTYPE_INVALID));
	// 即使保留着上次连接的地址，回放也不能注册到那个服务器。
	EXPECT_FALSE(RelayRouting::CanRegister(IClient::STATE_DEMOPLAYBACK, NETTYPE_IPV4));
}

TEST(TClientRelay, NonOnlineStatesNeverRegister)
{
	for(int State : {IClient::STATE_OFFLINE, IClient::STATE_CONNECTING, IClient::STATE_LOADING, IClient::STATE_QUITTING})
	{
		EXPECT_FALSE(RelayRouting::CanRegister(State, NETTYPE_INVALID));
		EXPECT_FALSE(RelayRouting::CanRegister(State, NETTYPE_IPV4));
	}
}

TEST(TClientRelay, OnlineNeedsAnAddressFamily)
{
	EXPECT_FALSE(RelayRouting::CanRegister(IClient::STATE_ONLINE, NETTYPE_INVALID));
	EXPECT_FALSE(RelayRouting::CanRegister(IClient::STATE_ONLINE, NETTYPE_TW7));
	EXPECT_FALSE(RelayRouting::CanRegister(IClient::STATE_ONLINE, NETTYPE_LINK_BROADCAST));
	EXPECT_TRUE(RelayRouting::CanRegister(IClient::STATE_ONLINE, NETTYPE_IPV4));
	EXPECT_TRUE(RelayRouting::CanRegister(IClient::STATE_ONLINE, NETTYPE_IPV6));
	EXPECT_TRUE(RelayRouting::CanRegister(IClient::STATE_ONLINE, NETTYPE_WEBSOCKET_IPV4));
	EXPECT_TRUE(RelayRouting::CanRegister(IClient::STATE_ONLINE, NETTYPE_WEBSOCKET_IPV6));
	EXPECT_TRUE(RelayRouting::CanRegister(IClient::STATE_ONLINE, NETTYPE_IPV4 | NETTYPE_TW7));
}
