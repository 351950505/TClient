/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CONTROLS_H
#define GAME_CLIENT_COMPONENTS_CONTROLS_H

#include <base/vmath.h>

#include <engine/client.h>
#include <engine/console.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CControls : public CComponent
{
public:
	float GetMinMouseDistance() const;
	float GetMaxMouseDistance() const;

	enum class EMouseInputType
	{
		ABSOLUTE,
		RELATIVE,
		AUTOMATED,
	};

	vec2 m_aMousePos[NUM_DUMMIES];
	vec2 m_aMousePosOnAction[NUM_DUMMIES];
	vec2 m_aTargetPos[NUM_DUMMIES];

	EMouseInputType m_aMouseInputType[NUM_DUMMIES];

	int m_aAmmoCount[NUM_WEAPONS];

	int64_t m_LastSendTime;
	CNetObj_PlayerInput m_aInputData[NUM_DUMMIES];
	CNetObj_PlayerInput m_aLastData[NUM_DUMMIES];
	int m_aInputDirectionLeft[NUM_DUMMIES];
	int m_aInputDirectionRight[NUM_DUMMIES];
	int m_aShowHookColl[NUM_DUMMIES];

	// 原版 Tater 输入保持独立；Ancient 动态量也会读取这里，不能被其他模式覆盖。
	CNetObj_PlayerInput m_aFastInput[NUM_DUMMIES] = {};
	bool m_FastInputHookAction = false;
	bool m_FastInputFireAction = false;

	// alpha 各模式分别保留两号输入和动作锁存；切换模式不覆盖其他模式缓存。
	// mode 0~7 = alpha 七模式（4 为历史空位），8~11 = BestClient 四模式（Best/Saiko/Delta/Cloud）。
	static constexpr int NUM_FAST_INPUT_MODES = 12;
	CNetObj_PlayerInput m_aaModeFastInput[NUM_FAST_INPUT_MODES][NUM_DUMMIES] = {};
	bool m_aModeFastInputHookAction[NUM_FAST_INPUT_MODES] = {};
	bool m_aModeFastInputFireAction[NUM_FAST_INPUT_MODES] = {};
	int m_ActiveFastInputMode = 0;

	CControls();
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	void OnConsoleInit() override;
	virtual void OnPlayerDeath();

	int SnapInput(int *pData);
	void ClampMousePos();
	void ResetInput(int Dummy);
	bool CheckNewInput();
	CNetObj_PlayerInput &GetFastInput(int Dummy) { return m_ActiveFastInputMode == 0 ? m_aFastInput[Dummy] : m_aaModeFastInput[m_ActiveFastInputMode][Dummy]; }
	const CNetObj_PlayerInput &GetFastInput(int Dummy) const { return m_ActiveFastInputMode == 0 ? m_aFastInput[Dummy] : m_aaModeFastInput[m_ActiveFastInputMode][Dummy]; }

private:
	static void ConKeyInputState(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputSet(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData);
};
#endif
