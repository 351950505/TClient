// Static regression for the scoreboard player-popup invite action.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8').replace(/\r\n/g, '\n');
const scoreboard = read('src/game/client/components/scoreboard.cpp');
const header = read('src/game/client/components/scoreboard.h');
const invite = scoreboard.slice(
  scoreboard.indexOf('const int InviterId ='),
  scoreboard.indexOf('pScoreboard->GameClient()->m_Tooltips.DoToolTip(&pPopupContext->m_InviteToTeamAction')
);
const actionRow = scoreboard.slice(
  scoreboard.indexOf('// TClient(2026-11-01):第二行动作'),
  scoreboard.indexOf('const float ButtonSize = 17.5f;')
);

assert(header.includes('CButtonContainer m_InviteToTeamAction;'));
assert(actionRow.includes('pPopupContext->m_InviteToTeamAction'));
assert.equal(actionRow.split('Container.VSplitLeft(ActionSpacing, nullptr, &Container);').length - 1, 2, 'skin/team/invite actions occupy the three slots');
assert(invite.includes('pScoreboard->GameClient()->m_aLocalIds[g_Config.m_ClDummy]'));
assert(invite.includes('pScoreboard->GameClient()->m_Teams.Team(InviterId)'));
assert(invite.includes('InviterTeam > TEAM_FLOCK && InviterTeam < pScoreboard->GameClient()->m_Teams.TeamSuper()'));
assert(invite.includes('Client()->State() == IClient::STATE_ONLINE'));
assert(invite.includes('FontIcon::USER'));
assert(invite.includes('Client.m_aName'));
assert(invite.includes('str_escape(&pDst, Client.m_aName, aInviteCommand + sizeof(aInviteCommand));'));
assert(invite.includes('pScoreboard->Console()->ExecuteLine(aInviteCommand, IConsole::CLIENT_ID_UNSPECIFIED);'));
assert(invite.includes('return CUi::POPUP_CLOSE_CURRENT;'));
assert(scoreboard.includes(String.raw`str_copy(aInviteCommand, "say /invite \"", sizeof(aInviteCommand));`));
assert(scoreboard.includes(String.raw`str_append(aInviteCommand, "\"");`));
assert(scoreboard.includes('m_Teams.Team(pPopupContext->m_ClientId)'));

for(const [language, translation] of [
  ['simplified_chinese.txt', '邀请加入我的队伍'],
  ['traditional_chinese.txt', '邀請加入我的隊伍'],
])
{
  const dictionary = read(`data/tclient/languages/${language}`);
  assert(dictionary.includes(`Invite to your team\n== ${translation}`), `${language} invite tooltip translation`);
}

console.log('PASS: scoreboard invite uses the active local team, escapes the target name, preserves join-team, and has both Chinese translations');
