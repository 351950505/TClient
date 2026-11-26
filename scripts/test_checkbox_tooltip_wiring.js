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

expect(entries.length === 142, `帮助表条目数应为 142，实际为 ${entries.length}`);
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
