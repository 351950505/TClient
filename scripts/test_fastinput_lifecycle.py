"""Validate fast-input connection lifecycle invariants in production source.

This source guard complements the isolated behavior fixtures. It ensures Cloud PreInput is
accepted before the dummy-message early return, stored per connection, and that disconnects
clear connection-scoped prediction state.
"""
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
GAME_H = (ROOT / "src/game/client/gameclient.h").read_text(encoding="utf-8")
GAME_CPP = (ROOT / "src/game/client/gameclient.cpp").read_text(encoding="utf-8")
CONTROLS_H = (ROOT / "src/game/client/components/controls.h").read_text(encoding="utf-8")
CONTROLS_CPP = (ROOT / "src/game/client/components/controls.cpp").read_text(encoding="utf-8")


def extract(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


checks: list[tuple[str, bool]] = []


def check(name: str, condition: bool) -> None:
    checks.append((name, condition))
    print(("PASS" if condition else "FAIL") + ": " + name)


on_message = extract(GAME_CPP, "void CGameClient::OnMessage(")
early_preinput = on_message.find("if(MsgId == NETMSGTYPE_SV_PREINPUT)")
dummy_return = on_message.find("if(Dummy)")
late_preinput = on_message.find("else if(MsgId == NETMSGTYPE_SV_PREINPUT)")
apply_preinputs = extract(GAME_CPP, "void CGameClient::ApplyPreInputs(")
client_reset = extract(GAME_CPP, "void CGameClient::CClientData::Reset()")
dummy_disconnect = extract(GAME_CPP, "void CGameClient::OnDummyDisconnect()")
controls_reset = extract(CONTROLS_CPP, "void CControls::OnReset()")
fast_input_reset = extract(CONTROLS_CPP, "void CControls::ResetFastInput(int Dummy)")
check_new_input = extract(CONTROLS_CPP, "bool CControls::CheckNewInput()")

check(
    "PreInput ring is partitioned by physical connection",
    "m_aaPreInputs[NUM_DUMMIES][200]" in GAME_H,
)
check(
    "PreInput is handled before the dummy early return",
    early_preinput >= 0 and dummy_return >= 0 and early_preinput < dummy_return,
)
early_block = on_message[early_preinput:dummy_return] if early_preinput >= 0 and dummy_return >= 0 else ""
check(
    "early PreInput handling records connection capability",
    "m_aReceivedPreInput[Conn] = true" in early_block,
)
check(
    "early PreInput handling writes the connection-specific ring",
    "m_aaPreInputs[Conn]" in early_block,
)
check(
    "PreInput is not handled again after the dummy early return",
    late_preinput < 0,
)
check(
    "prediction reads PreInput from the active physical connection",
    "m_aaPreInputs[g_Config.m_ClDummy]" in apply_preinputs,
)
check(
    "client reset invalidates every connection-specific PreInput ring",
    "m_aaPreInputs" in client_reset and client_reset.count("for(auto &") >= 2,
)
check(
    "F smoothing state is partitioned by connection",
    all(name in GAME_H for name in ("m_aFInputExtra[NUM_DUMMIES]", "m_aFInputGameTick[NUM_DUMMIES]", "m_aFInputLastTime[NUM_DUMMIES]", "m_aFInputValid[NUM_DUMMIES]")),
)
check(
    "controls expose a per-connection fast-input reset",
    "void ResetFastInput(int Dummy);" in CONTROLS_H and "void CControls::ResetFastInput(int Dummy)" in CONTROLS_CPP,
)
check(
    "fast-input action latches are partitioned by physical connection",
    "m_FastInputHookAction[NUM_DUMMIES]" in CONTROLS_H
    and "m_FastInputFireAction[NUM_DUMMIES]" in CONTROLS_H
    and "m_aModeFastInputHookAction[NUM_FAST_INPUT_MODES][NUM_DUMMIES]" in CONTROLS_H
    and "m_aModeFastInputFireAction[NUM_FAST_INPUT_MODES][NUM_DUMMIES]" in CONTROLS_H,
)
check(
    "per-connection fast-input reset clears only that connection action latches",
    all(token in fast_input_reset for token in (
        "m_FastInputHookAction[Dummy] = false",
        "m_FastInputFireAction[Dummy] = false",
        "aModeHookActions[Dummy] = false",
        "aModeFireActions[Dummy] = false",
    )),
)
check(
    "full controls reset clears every fast-input connection slot",
    "ResetFastInput(0);" in controls_reset and "ResetFastInput(1);" in controls_reset,
)
check(
    "every fast-input mode transition forces one reprediction",
    re.search(r"bool\s+NewInput\s*=\s*Mode\s*!=\s*m_ActiveFastInputMode\s*;", check_new_input) is not None,
)
check(
    "dummy disconnect clears connection capability and ring",
    "m_aReceivedPreInput[1] = false" in dummy_disconnect and "m_aaPreInputs[1]" in dummy_disconnect,
)
check(
    "dummy disconnect releases ordinary and fast input for connection 1",
    "m_Controls.ResetInput(1);" in dummy_disconnect
    and "m_Controls.m_aInputData[1].m_Hook = 0;" in dummy_disconnect
    and "m_Controls.ResetFastInput(1);" in dummy_disconnect,
)
check(
    "dummy disconnect clears fast-input and Ancient connection state",
    "m_Controls.ResetFastInput(1);" in dummy_disconnect
    and "m_aAncientInputState[1] = {}" in dummy_disconnect
    and "m_aLightAncientInputState[1] = {}" in dummy_disconnect,
)
check(
    "dummy disconnect clears F smoothing state for connection 1",
    all(name in dummy_disconnect for name in ("m_aFInputExtra[1]", "m_aFInputGameTick[1]", "m_aFInputLastTime[1]", "m_aFInputValid[1]")),
)
check(
    "dummy disconnect clears every actual dummy send cache",
    all(name in dummy_disconnect for name in ("m_DummyInput = {}", "m_HammerInput = {}", "m_DummyFire = 0")),
)

improved_block_start = GAME_CPP.index("// New antiping smoothing")
improved_block_end = GAME_CPP.index("void CGameClient::UpdateRenderedCharacters()", improved_block_start)
improved_antiping = GAME_CPP[improved_block_start:improved_block_end]
player_loop = improved_antiping[improved_antiping.index("for(int i = 0; i < MAX_CLIENTS; i++)"):]
check(
    "Improved AntiPing computes one loop-invariant game tick without static state",
    "static int s_PrevGameTick" not in improved_antiping
    and player_loop.find("int GameTick =") < 0
    and improved_antiping.find("const int GameTick =") >= 0,
)

failures = [name for name, passed in checks if not passed]
print(f"checks={len(checks)} failures={len(failures)}")
if failures:
    print("FAILED INVARIANTS:")
    for name in failures:
        print(f"- {name}")
    raise SystemExit(1)
raise SystemExit(0)
