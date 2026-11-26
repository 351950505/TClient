#!/usr/bin/env node
'use strict';
// 悬停说明（checkbox tooltip）接线回归：
// 1) 通用复选框把配置项地址交给 DoToolTipCheckBoxHelp，且只在 TClient / ME 页生效；
// 2) 帮助表每个条目的英文原文在简体词典都必须有非空短译文；
// 3) 表不为空（空表 = 功能没真正接入）；
// 4) 包装开关（上行复制两行）与冻结计数第二档的特例提示仍在。
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const READ = (...p) => fs.readFileSync(path.join(ROOT, ...p), 'utf8');

function expect(cond, msg) {
	if(!cond) {
		console.error('FAIL: ' + msg);
		process.exitCode = 1;
	}
}

// ---- 表条目与文案 ----
const HELP_CPP = READ('src', 'game', 'client', 'components', 'menus_checkbox_help.cpp');
const ENTRY_RE = /\{\s*&g_Config\.m_(\w+)\s*,\s*"((?:\\.|[^"\\])*)"\s*\}/g;
const entries = [];
let m;
while((m = ENTRY_RE.exec(HELP_CPP)))
	entries.push({ name: m[1], help: m[2].replace(/\\"/g, '"').replace(/\\\\/g, '\\') });

expect(entries.length === 143, `帮助表条目数应为 143，实际为 ${entries.length}`);
expect(entries.length === [...new Set(entries.map(e => e.name))].length, '帮助表存在重复配置项条目');

// ---- 词典（data/tclient/languages，格式：可选 [context] 段 + 英文行 + "== 译文"）----
// 与 check_translations.py 相同的成对解析
function parseDict(rel) {
	const lines = READ(...rel).split(/\r?\n/);
	const dict = new Map();
	let context = '';
	for(let i = 0; i < lines.length; i++) {
		const line = lines[i];
		if(/^\[[^\]]+\]$/.test(line)) {
			context = line.slice(1, -1);
			continue;
		}
		if(line.startsWith('##### ') && line.endsWith(' #####')) {
			context = '';
			continue;
		}
		if(!line) {
			// 空行结束当前 context（与 check_translations.py 一致）
			context = '';
			continue;
		}
		if(line.startsWith('#'))
			continue;
		if(line.startsWith('== '))
			continue;
		// 英文原文行：下一行必须是 "== 译文"
		if(i + 1 < lines.length && lines[i + 1].startsWith('== ')) {
			const translation = lines[i + 1].slice(3);
			dict.set(context + '\n' + line, translation);
		}
	}
	return dict;
}

const SC = parseDict(['data', 'tclient', 'languages', 'simplified_chinese.txt']);

for(const entry of entries) {
	const sc = SC.get('\n' + entry.help);
	expect(sc !== undefined && sc.length > 0, `简体词典缺少 "${entry.help}" (${entry.name}) 的非空译文`);
	if(sc !== undefined)
		expect(Array.from(sc).length <= 70, `简体悬停说明过长 (${entry.name})：${Array.from(sc).length} 字`);
}

// ---- 菜单接线 ----
const MENUS_CPP = READ('src', 'game', 'client', 'components', 'menus.cpp');
expect(/int CMenus::DoButton_CheckBoxAutoVMarginAndSet[\s\S]*?DoToolTipCheckBoxHelp\(pId, &CheckBoxRect, pValue\);\s*\n\s*return Logic;/.test(MENUS_CPP),
	'DoButton_CheckBoxAutoVMarginAndSet 未把配置项交给 DoToolTipCheckBoxHelp');
expect(/void CMenus::DoToolTipCheckBoxHelp[\s\S]*?SETTINGS_TCLIENT[\s\S]*?SETTINGS_ME[\s\S]*?FindCheckBoxHelpText/.test(MENUS_CPP),
	'DoToolTipCheckBoxHelp 缺少 TClient/ME 页门控或帮助表查找');
expect(MENUS_CPP.includes('#include <game/client/components/menus_checkbox_help.h>'), 'menus.cpp 未包含帮助表头文件');
// 接线只发生在 DoButton_CheckBoxAutoVMarginAndSet 一处（上方正则已锁定），
// 编号/普通复选框与带勾选值的 DoButton_CheckBox 天然不查表

const MENUS_H = READ('src', 'game', 'client', 'components', 'menus.h');
expect(MENUS_H.includes('void DoToolTipCheckBoxHelp(const void *pId, const CUIRect *pRect, const int *pValue);'), 'menus.h 缺少 DoToolTipCheckBoxHelp 声明');

// ---- 特例：包装开关与冻结计数 ----
const TCLIENT_CPP = READ('src', 'game', 'client', 'components', 'tclient', 'menus_tclient.cpp');
expect(/s_DupUpstreamToggle[\s\S]{0,200}m_Tooltips\.DoToolTip\(&s_DupUpstreamToggle/.test(TCLIENT_CPP), '上行复制开关缺少悬停说明');
expect(/s_DupSpreadToggle[\s\S]{0,200}m_Tooltips\.DoToolTip\(&s_DupSpreadToggle/.test(TCLIENT_CPP), '错峰铺开开关缺少悬停说明');
expect(/s_CountFrozenText[\s\S]{0,220}m_Tooltips\.DoToolTip\(&s_CountFrozenText/.test(TCLIENT_CPP), '冻结计数第二档缺少悬停说明');
expect(TCLIENT_CPP.includes('DoToolTipCheckBoxHelp(&g_Config.m_TcShowFrozenText'), '冻结存活文字行未走帮助表');

const CONFIG = READ('src', 'engine', 'shared', 'config_variables_tclient.h');
const OUTLINES_CPP = READ('src', 'game', 'client', 'components', 'tclient', 'outlines.cpp');
const outlineMenuStart = TCLIENT_CPP.indexOf('// ***** Tile Outlines ***** //');
const outlineMenuEnd = TCLIENT_CPP.indexOf('s_SectionBoxes.back().h', outlineMenuStart);
const outlineSection = TCLIENT_CPP.slice(outlineMenuStart, outlineMenuEnd);
expect(/MACRO_CONFIG_INT\(TcOutlineTransparent, tc_outline_transparent, 0, 0, 1, CFGFLAG_CLIENT \| CFGFLAG_SAVE,/.test(CONFIG), '透明模式默认关闭且需主动开启');
expect(/MACRO_CONFIG_INT\(TcOutlineOpacity, tc_outline_opacity, 35, 0, 100, CFGFLAG_CLIENT \| CFGFLAG_SAVE,/.test(CONFIG), '透明模式的不透明度默认约 35% 且范围 0–100%');
expect(/if\(g_Config\.m_TcOutlineTransparent\)\s*\{[^}]*DoScrollbarOption\(&g_Config\.m_TcOutlineOpacity, &g_Config\.m_TcOutlineOpacity, &Button, TCLocalize\("Outline opacity multiplier"\), 0, 100,/.test(outlineSection), '透明模式开启后显示可调不透明度滑条');
expect(/const float Opacity = g_Config\.m_TcOutlineTransparent \? g_Config\.m_TcOutlineOpacity \/ 100\.0f : 1\.0f;\s*if\(Opacity <= 0\.0f\)\s*return;/.test(OUTLINES_CPP) && OUTLINES_CPP.indexOf('if(Opacity <= 0.0f)') < OUTLINES_CPP.indexOf('Graphics()->QuadsBegin()'), '关闭模式保留原不透明度，0% 时跳过绘制');
expect(/SetColor\(color_cast<ColorRGBA>\(ColorHSLA\(Config\.m_Color, true\)\)\.WithMultipliedAlpha\(Opacity\)\)/.test(OUTLINES_CPP), '百分比只缩放现有轮廓色的 alpha，保留 RGB 与保存颜色');
for(const lang of ['simplified_chinese.txt','traditional_chinese.txt']) {
  const dict = READ('data', 'tclient', 'languages', lang);
  for(const key of ['Tile outline transparent mode','Outline opacity multiplier','Scale outline opacity without changing saved colors.'])
    expect(dict.includes(`\n${key}\n== `), `${lang} 缺少透明模式翻译: ${key}`);
}
const DIRECT_HELP_KEYS = [
	'Resends packets until the server confirms them, so hits still register on bad connections.',
	'Sends the copies a few milliseconds apart instead of back to back.',
	'Shows how many teammates are frozen instead of how many are alive.',
];
for(const key of DIRECT_HELP_KEYS) {
	const sc = SC.get('\n' + key);
	expect(sc !== undefined && sc.length > 0, `简体词典缺少直接悬停提示 "${key}"`);
	if(sc !== undefined)
		expect(Array.from(sc).length <= 70, `简体直接悬停提示过长：${Array.from(sc).length} 字`);
}

// ---- 测试与构建注册 ----
const CMAKE = READ('CMakeLists.txt');
expect(CMAKE.includes('tclient_checkbox_help_test.cpp'), 'testrunner 未注册 tclient_checkbox_help_test.cpp');

if(!process.exitCode)
	console.log(`OK: checkbox hover help wiring verified (${entries.length} entries, Simplified Chinese complete)`);
