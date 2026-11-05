#include <base/logger.h>
#include <base/str.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

TEST(FutureLogger, FlushesPendingAndForwardsNewMessages)
{
	CFutureLogger Future;
	CLogMessage Message = {};
	Message.m_Level = LEVEL_INFO;
	str_copy(Message.m_aLine, "pending");
	Future.Log(&Message);

	auto pLogger = std::make_shared<CMemoryLogger>();
	Future.Set(pLogger);
	str_copy(Message.m_aLine, "forwarded");
	Future.Log(&Message);
	EXPECT_EQ(pLogger->ConcatenatedLines(), "pending\nforwarded");
}

namespace
{
class CCountingLogger : public ILogger
{
public:
	static constexpr int NUM_MESSAGES = 1024;
	std::array<std::atomic<int>, NUM_MESSAGES> m_aCounts{};
	std::atomic<int> m_Invalid{0};
	std::atomic<bool> m_Finished{false};

	void Log(const CLogMessage *pMessage) override
	{
		const int Index = pMessage->m_LineMessageOffset;
		if(Index >= 0 && Index < NUM_MESSAGES)
			m_aCounts[Index].fetch_add(1, std::memory_order_relaxed);
		else
			m_Invalid.fetch_add(1, std::memory_order_relaxed);
	}

	void GlobalFinish() override
	{
		m_Finished.store(true, std::memory_order_relaxed);
	}
};
}

TEST(FutureLogger, ConcurrentPublicationDeliversEachMessageOnce)
{
	CFutureLogger Future;
	auto pLogger = std::make_shared<CCountingLogger>();
	std::atomic<int> Ready{0};
	std::atomic<bool> Start{false};
	std::vector<std::thread> vWorkers;
	for(int Worker = 0; Worker < 4; ++Worker)
	{
		vWorkers.emplace_back([&, Worker] {
			CLogMessage Message = {};
			Message.m_Level = LEVEL_INFO;
			Message.m_LineMessageOffset = Worker * 256;
			Future.Log(&Message);
			Ready.fetch_add(1, std::memory_order_release);
			while(!Start.load(std::memory_order_acquire))
				std::this_thread::yield();
			for(int Index = 1; Index < 256; ++Index)
			{
				Message.m_LineMessageOffset = Worker * 256 + Index;
				Future.Log(&Message);
			}
		});
	}
	while(Ready.load(std::memory_order_acquire) != 4)
		std::this_thread::yield();
	Start.store(true, std::memory_order_release);
	Future.Set(pLogger);
	for(auto &Worker : vWorkers)
		Worker.join();
	for(const auto &Count : pLogger->m_aCounts)
		EXPECT_EQ(Count.load(std::memory_order_relaxed), 1);
	EXPECT_EQ(pLogger->m_Invalid.load(), 0);
	Future.GlobalFinish();
	EXPECT_TRUE(pLogger->m_Finished.load());
}
