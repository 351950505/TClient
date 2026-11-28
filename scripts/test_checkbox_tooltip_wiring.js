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

expect(entries.length === 144, `帮助表条目数应为 144，实际为 ${entries.length}`);
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
expect(/MACRO_CONFIG_INT\(TcOutlineTransparent, tc_outline_transparent, 0, 0, 1, CFGFLAG_CLIENT \| CFGFLAG_SAVE,/.test(CONFIG), '冻结轮廓隐藏开关默认关闭');
expect(!/\bTcOutlineOpacity\b|tc_outline_opacity/.test(CONFIG), '隐藏冻结轮廓时不应保留无效的不透明度配置');
expect(!/TcOutlineOpacity|Outline opacity multiplier/.test(outlineSection), '隐藏冻结轮廓时不显示无效的不透明度滑条');
const outlineRenderStart = OUTLINES_CPP.indexOf('void COutlines::OnRender()');
const outlineRender = OUTLINES_CPP.slice(outlineRenderStart);
expect(outlineRenderStart >= 0 && outlineRender.includes('MakeFreezeOutlineGeometry'), 'Outline strokes use shared edge geometry');
expect(outlineRender.includes('Type == OUTLINE_FREEZE') && outlineRender.includes('g_Config.m_TcOutlineTransparent'), 'Freeze cutout retains its outline-width geometry');
expect(/if\(g_Config\.m_TcOutlineTransparent && Type == OUTLINE_FREEZE\)\s*continue;[\s\S]*?QuadsDrawTL\(/.test(outlineRender), '镂空模式必须在绘制前跳过原冻结描边');
expect(!OUTLINES_CPP.includes('m_TcOutlineOpacity'), 'Freeze-band cutout has no unrelated opacity slider');
const mapLayers = READ('src', 'game', 'client', 'components', 'maplayers.cpp');
expect(mapLayers.includes('m_CutoutFreezeOutlines') && mapLayers.includes('m_FreezeOutlineMap') && mapLayers.includes('m_FreezeOutlineWidth'), 'Pass the enabled outline map and configured width into entity tile rendering');
const renderLayer = READ('src', 'game', 'map', 'render_layer.cpp');
expect(renderLayer.includes('RenderTilemapWithFreezeCutout') && renderLayer.includes('ShouldCutoutFreezeOutlines'), 'Apply tile cutout to both buffered and immediate entity layer paths');
const renderMap = READ('src', 'game', 'map', 'render_map.cpp');
expect(/RenderMap\(\)->RenderTilemap\(m_pTiles,[^;]*&Params\.m_FreezeOutlineMap,\s*Params\.m_FreezeOutlineWidth,/.test(renderLayer) &&
	/TILE_FREEZE \|\|[^;]*?TILE_DFREEZE \|\|[^;]*?TILE_LFREEZE[^;]*?const auto Cutout = MakeFreezeOutlineGeometry\(\*pFreezeOutlineMap, x, y, FREEZE_OUTLINE_FREEZE, FreezeOutlineWidth,[^;]*;\s*if\(Cutout\.m_NumBand > 0\)[\s\S]*?for\(int i = 0; i < Cutout\.m_NumInterior; \+\+i\)\s*\{\s*const auto &Rect = Cutout\.m_aInterior\[i\];\s*DrawSubTile\(Rect\.[^;]+;\s*\}\s*DidDrawCutout = true;[\s\S]*?if\(!DidDrawCutout &&/.test(renderMap),
	'Game/Front 只用冻结宽度裁实际冻结贴图区块，不再补画完整冻结贴图');
const outlineHelp = 'Cut the freeze texture beneath exposed outline-width strips so the background shows through. Frozen tiles still freeze your Tee.';
expect(entries.some(entry => entry.name === 'TcOutlineTransparent' && entry.help === outlineHelp), 'Help describes the cutout while preserving gameplay freeze');
for(const key of ['Cut out freeze borders', outlineHelp])
  expect(SC.has('\n' + key), `简体词典缺少冻结边带镂空说明: ${key}`);
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
