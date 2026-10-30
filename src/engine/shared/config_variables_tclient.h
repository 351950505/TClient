// This file can be included several times.

#ifndef MACRO_CONFIG_INT
#error "The config macros must be defined"
#define MACRO_CONFIG_INT(Tcme, ScriptName, Def, Min, Max, Save, Desc) ;
#define MACRO_CONFIG_FLOAT(Tcme, ScriptName, Def, Min, Max, Save, Desc) ;
#define MACRO_CONFIG_COL(Tcme, ScriptName, Def, Save, Desc) ;
#define MACRO_CONFIG_STR(Tcme, ScriptName, Len, Def, Save, Desc) ;
#endif

// [DUP-UPSTREAM] 上行复制上限：0=关，1=固定单发，2=最多双发，3=最多三发。
// 上限 2/3 使用单发独立探针的未确认率选择份数；它包含双向链路影响，不是纯上行丢包率。
MACRO_CONFIG_INT(ClDupUpstream, cl_dup_upstream, 3, 0, 3, CFGFLAG_CLIENT | CFGFLAG_SAVE, "自适应上行复制上限（0=关闭，1=单发，2=最多双发，3=最多三发；依据独立探针未确认率）")
// [DUP-UPSTREAM 稳定化] 下限份数：探针没样本时按这个份数发。默认1=链路正常就单发（双发只在真丢包时才上，避免多出来的包引发双锤等问题）
MACRO_CONFIG_INT(ClDupUpstreamMin, cl_dup_upstream_min, 1, 0, 3, CFGFLAG_CLIENT | CFGFLAG_SAVE, "上行复制下限份数（默认1=正常时单发，只有探针测到未确认才升；设2=始终至少双发）")
// 探针未确认率升到 low 就上双发，升到 high 就上三发（单位：百分比）
MACRO_CONFIG_INT(ClDupUpstreamLowPct, cl_dup_upstream_low_pct, 1, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "未确认率达到该百分比就升到双发（默认1）")
MACRO_CONFIG_INT(ClDupUpstreamHighPct, cl_dup_upstream_high_pct, 5, 1, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "未确认率达到该百分比就升到三发（默认5）")
// 降档滞回：指标必须连续干净够久才降档，防止份数在 1/2/3 之间来回跳
MACRO_CONFIG_INT(ClDupUpstreamProbe, cl_dup_upstream_probe, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "独立探针开关（1=即使不复制也测上行未确认率，方便先测后开；0=只在启用复制时才测）")
MACRO_CONFIG_INT(ClDupUpstreamHoldSec, cl_dup_upstream_hold_sec, 30, 0, 300, CFGFLAG_CLIENT | CFGFLAG_SAVE, "降档前需要连续无丢包保持的秒数（默认30，防抖动）")
// [DUP-UPSTREAM 双模式] A/B 两套发包方式，随时切换防玩家出问题：0=背靠背（原版），1=错峰铺开（副本按 spread_ms 间隔错开发，抗突发丢包）
MACRO_CONFIG_INT(ClDupSendMode, cl_dup_send_mode, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "上行副本发送方式：0=背靠背（原版），1=错峰铺开（抗突发丢包，副本在约15毫秒内铺开）")
MACRO_CONFIG_INT(ClDupSpreadMs, cl_dup_spread_ms, 4, 1, 15, CFGFLAG_CLIENT | CFGFLAG_SAVE, "错峰模式下副本间隔毫秒（默认4；仅 cl_dup_send_mode=1 时生效）")

// [PING-WHEEL] LoL 式地图信号：按住键出轮盘，选信号在准星世界坐标插地图标记；走聊天通道（⟨P:...⟩ 前缀）同步给同服 TClient 玩家，服务端零改动
MACRO_CONFIG_INT(TcPingwheel, tc_pingwheel, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "地图信号轮盘（LoL式ping；0=关闭，同时不再解析他人信号）")
MACRO_CONFIG_FLOAT(TcQuickZoom, tc_quick_zoom_level, 15.0f, 0.0f, 20.0f, CFGFLAG_CLIENT | CFGFLAG_SAVE, "一键缩放的目标缩放（与默认缩放同刻度，10=原始）")

// [SPEC-CURSOR] 观战指针常驻：自由视角显示鼠标指针（5 秒不动淡出、移动即恢复），被观战者暂停时指针也不隐藏
MACRO_CONFIG_INT(TcSpecCursorAlways, tc_spec_cursor_always, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "观战指针常驻（1=自由视角常驻显示鼠标指针+被观战者暂停不隐藏；0=DDNet 原版行为）")

MACRO_CONFIG_STR(TcPingwheelLayout, tc_pingwheel_layout, 16, "0,1,2,3,4,5,6,7", CFGFLAG_CLIENT | CFGFLAG_SAVE, "轮盘扇区→信号顺序（TC 设置 Ping 页可调）")

#if defined(CONF_FAMILY_WINDOWS)
MACRO_CONFIG_INT(TcAllowAnyRes, tc_allow_any_res, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Whether to allow any resolution in game when zoom is allowed (buggy on Windows)")
#else
MACRO_CONFIG_INT(TcAllowAnyRes, tc_allow_any_res, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Whether to allow any resolution in game when zoom is allowed (buggy on Windows)")
#endif

MACRO_CONFIG_INT(TcShowChatClient, tc_show_chat_client, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show chat messages from the client such as echo")

MACRO_CONFIG_INT(TcShowFrozenText, tc_frozen_tees_text, 0, 0, 2, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show how many tees in your team are currently frozen. (0 - off, 1 - show alive, 2 - show frozen)")
MACRO_CONFIG_INT(TcShowFrozenHud, tc_frozen_tees_hud, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show frozen tee HUD")
MACRO_CONFIG_INT(TcShowFrozenHudSkins, tc_frozen_tees_hud_skins, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Use ninja skin, or darkened skin for frozen tees on hud")

MACRO_CONFIG_INT(TcFrozenHudTeeSize, tc_frozen_tees_size, 15, 8, 20, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Size of tees in frozen tee hud. (Default : 15)")
MACRO_CONFIG_INT(TcFrozenMaxRows, tc_frozen_tees_max_rows, 1, 1, 6, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Maximum number of rows in frozen tee HUD display")
MACRO_CONFIG_INT(TcFrozenHudTeamOnly, tc_frozen_tees_only_inteam, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Only render frozen tee HUD display while in team")

MACRO_CONFIG_INT(TcNameplatePingCircle, tc_nameplate_ping_circle, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Shows a circle to indicate ping in the nameplate")
MACRO_CONFIG_INT(TcNameplateCountry, tc_nameplate_country, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Shows the country flag in the nameplate")
MACRO_CONFIG_INT(TcNameplateSkins, tc_nameplate_skins, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Shows skin names in nameplates, good for finding missing skins")

MACRO_CONFIG_INT(TcFakeCtfFlags, tc_fake_ctf_flags, 0, 0, 2, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Shows fake CTF flags on people (0 = off, 1 = red, 2 = blue)")

MACRO_CONFIG_INT(TcLimitMouseToScreen, tc_limit_mouse_to_screen, 0, 0, 2, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Limit mouse to screen boundaries")
MACRO_CONFIG_INT(TcScaleMouseDistance, tc_scale_mouse_distance, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Improve mouse precision by scaling max distance to 1000")

MACRO_CONFIG_INT(TcHammerRotatesWithCursor, tc_hammer_rotates_with_cursor, 0, 0, 2, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Allow your hammer to rotate like other weapons")

MACRO_CONFIG_INT(TcMiniVoteHud, tc_mini_vote_hud, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "When enabled makes the vote UI small")

// Anti Latency Tools
MACRO_CONFIG_INT(TcRemoveAnti, tc_remove_anti, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Removes some amount of antiping & player prediction in freeze")
MACRO_CONFIG_INT(TcUnfreezeLagTicks, tc_remove_anti_ticks, 5, 0, 20, CFGFLAG_CLIENT | CFGFLAG_SAVE, "The biggest amount of prediction ticks that are removed")
MACRO_CONFIG_INT(TcUnfreezeLagDelayTicks, tc_remove_anti_delay_ticks, 25, 5, 150, CFGFLAG_CLIENT | CFGFLAG_SAVE, "How many ticks it takes to remove the maximum prediction after being frozen")

MACRO_CONFIG_INT(TcUnpredOthersInFreeze, tc_unpred_others_in_freeze, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dont predict other players if you are frozen")
MACRO_CONFIG_INT(TcPredMarginInFreeze, tc_pred_margin_in_freeze, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Enable changing prediction margin while frozen")
MACRO_CONFIG_INT(TcPredMarginInFreezeAmount, tc_pred_margin_in_freeze_amount, 15, 0, 2000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Set what your prediction margin while frozen should be")

MACRO_CONFIG_INT(TcShowOthersGhosts, tc_show_others_ghosts, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show ghosts for other players in their unpredicted position")
MACRO_CONFIG_INT(TcSwapGhosts, tc_swap_ghosts, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show predicted players as ghost and normal players as unpredicted")
MACRO_CONFIG_INT(TcHideFrozenGhosts, tc_hide_frozen_ghosts, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Hide Ghosts of other players if they are frozen")

MACRO_CONFIG_INT(TcPredGhostsAlpha, tc_pred_ghosts_alpha, 100, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Alpha of predicted ghosts (0-100)")
MACRO_CONFIG_INT(TcUnpredGhostsAlpha, tc_unpred_ghosts_alpha, 50, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Alpha of unpredicted ghosts (0-100)")
MACRO_CONFIG_INT(TcRenderGhostAsCircle, tc_render_ghost_as_circle, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Render Ghosts as circles instead of tee")

MACRO_CONFIG_INT(TcShowCenter, tc_show_center, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Draws lines to show the center of your screen/hitbox")
MACRO_CONFIG_INT(TcShowCenterWidth, tc_show_center_width, 0, 0, 20, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Center lines width (enabled by tc_show_center)")
MACRO_CONFIG_COL(TcShowCenterColor, tc_show_center_color, 1694498688, CFGFLAG_CLIENT | CFGFLAG_SAVE | CFGFLAG_COLALPHA, "Center lines color (enabled by tc_show_center)") // transparent red

MACRO_CONFIG_INT(TcFastInput, tc_fast_input, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Uses input for prediction before the next tick")
MACRO_CONFIG_INT(TcFastInputAmount, tc_fast_input_amount, 20, 1, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "How many milliseconds fast input will apply")
MACRO_CONFIG_INT(TcFastInputOthers, tc_fast_input_others, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Apply fast input to other tees")
MACRO_CONFIG_INT(TcFastInputRepredictHook, tc_fast_input_repredict_hook, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Repredict fast input hook when the direction changes (requires Sub-Tick aiming disabled)")
// 快速输入模式：0=Tater(原版) 1=Meow 2=Snow 3=Next 5=Salatik 6=Ancient 7=F（对齐 MeowClient-Next alpha）
// 8=Best 9=Saiko 10=Delta 11=Cloud（2026-10-29 自 BestClient 移植，见 交接摘要 §172）
MACRO_CONFIG_INT(TcFastInputMode, tc_fast_input_mode, 0, 0, 11, CFGFLAG_CLIENT | CFGFLAG_SAVE, "快速输入模式：0=Tater 1=Meow 2=Snow 3=Next 5=Salatik 6=Ancient 7=F 8=Best 9=Saiko 10=Delta 11=Cloud")
// 各模式的量均先换算为 tick，不是秒；Tater/Ancient/Hook 的毫秒值除以 20。
// Meow 是原生 float ticks（对齐 alpha 的 meow_input，0.0f~5.0f，默认 0.0f）；
// Snow/Next/Salatik 用百分之一 tick 兼容整数配置；F 用千分之一 tick。
MACRO_CONFIG_FLOAT(TcMeowInput, meow_input, 0.0f, 0.0f, 5.0f, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Meow 预测量（tick，0.0~5.0）")
// 旧版整数键 tc_meow_input（百分之一 tick）不在此声明，避免与原生 meow_input 混淆；
// CGameClient 手动注册 tc_meow_input 别名做迁移。
MACRO_CONFIG_INT(TcMeowInputOthers, tc_meow_input_others, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Meow 应用到其他玩家")
MACRO_CONFIG_INT(TcSnowInput, tc_snow_input, 0, 0, 500, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Snow 预测量（百分之一 tick，0~500）")
MACRO_CONFIG_INT(TcSnowInputHookAmount, tc_snow_input_hook_amount, 37, 1, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Snow 钩子预测量（毫秒，1~100）")
MACRO_CONFIG_INT(TcSnowInputHookAmountEnabled, tc_snow_input_hook_amount_enabled, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Snow 钩子交互时使用独立毫秒值")
MACRO_CONFIG_INT(TcSnowInputOthers, tc_snow_input_others, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Snow 应用到其他玩家")
MACRO_CONFIG_INT(TcNextInput, tc_next_input, 0, 0, 500, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Next 预测量（百分之一 tick，0~500）")
MACRO_CONFIG_INT(TcNextInputOthers, tc_next_input_others, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Next 应用到其他玩家")
MACRO_CONFIG_INT(TcFInput, tc_f_input, 0, 0, 5000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "F 预测量（千分之一 tick，0~5000）")
MACRO_CONFIG_INT(TcFInputOthers, tc_f_input_others, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "F 应用到其他玩家")
MACRO_CONFIG_INT(TcAncientInput, tc_ancient_input, 20, 1, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Ancient 预测量（毫秒，1~100）")
MACRO_CONFIG_INT(TcAncientInputOthers, tc_ancient_input_others, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Ancient 应用到其他玩家")
MACRO_CONFIG_INT(TcAncientInputOthersAmount, tc_ancient_input_others_amount, 12, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Ancient 其他玩家预测量（毫秒，0~100，不超过本机设置）")
MACRO_CONFIG_INT(TcSalatikInputOffset, tc_salatik_input_offset, 0, 0, 1000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Salatik 预测量（百分之一 tick，0~1000）")
MACRO_CONFIG_INT(TcSalatikInputSmoothing, tc_salatik_input_smoothing, 0, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Salatik 预测量平滑系数（0~100）")
MACRO_CONFIG_INT(TcSalatikInputLatencyComp, tc_salatik_input_latency_comp, 0, 0, 50, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Salatik 延迟补偿（0~50）")
MACRO_CONFIG_INT(TcSalatikInputInterpolation, tc_salatik_input_interpolation, 1, 1, 3, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Salatik 插值：1=线性，2=三次，3=平滑")
MACRO_CONFIG_INT(TcSalatikInputOthers, tc_salatik_input_others, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Salatik 应用到其他玩家")
// 2026-10-29 自 BestClient 移植的四模式；公式对齐 BestClient inputs.cpp/cloud_input.cpp。
// Best 与 Salatik 公式同构（复用 FastInput::SalatikAmount/SalatikIntra）；Cloud 本机按小数 tick 偏移、他人固定 1 tick。
MACRO_CONFIG_INT(TcBestInput, tc_best_input, 0, 0, 1000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Best 预测量（百分之一 tick，0~1000）")
MACRO_CONFIG_INT(TcBestInputSmoothing, tc_best_input_smoothing, 0, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Best 预测量平滑系数（0~100）")
MACRO_CONFIG_INT(TcBestInputLatencyComp, tc_best_input_latency_comp, 0, 0, 50, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Best 延迟补偿（0~50）")
MACRO_CONFIG_INT(TcBestInputInterpolation, tc_best_input_interpolation, 1, 1, 3, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Best 插值：1=线性，2=三次，3=平滑")
MACRO_CONFIG_INT(TcBestInputOthers, tc_best_input_others, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Best 应用到其他玩家")
MACRO_CONFIG_INT(TcSaikoInput, tc_saiko_input, 0, 0, 500, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Saiko 预测量（百分之一 tick，0~500）")
MACRO_CONFIG_INT(TcSaikoInputOthers, tc_saiko_input_others, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Saiko 应用到其他玩家")
MACRO_CONFIG_INT(TcDeltaInput, tc_delta_input, 0, 0, 500, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Delta 预测量（百分之一 tick，0~500）")
MACRO_CONFIG_INT(TcDeltaInputOthers, tc_delta_input_others, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Delta 应用到其他玩家")
MACRO_CONFIG_INT(TcCloudInput, tc_cloud_input, 0, 0, 500, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Cloud 预测量（百分之一 tick，0~500）")
MACRO_CONFIG_INT(TcCloudInputOthers, tc_cloud_input_others, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Cloud 应用到其他玩家")

MACRO_CONFIG_INT(TcAntiPingImproved, tc_antiping_improved, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Different antiping smoothing algorithm, not compatible with cl_antiping_smooth")
MACRO_CONFIG_INT(TcAntiPingNegativeBuffer, tc_antiping_negative_buffer, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Helps in Gores. Allows internal certainty value to be negative which causes more conservative prediction")
MACRO_CONFIG_INT(TcAntiPingStableDirection, tc_antiping_stable_direction, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Predicts optimistically along the tees stable axis to reduce delay in gaining overall stability")
MACRO_CONFIG_INT(TcAntiPingUncertaintyScale, tc_antiping_uncertainty_scale, 150, 25, 400, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Determines uncertainty duration as a factor of ping, 100 = 1.0")

MACRO_CONFIG_INT(TcColorFreeze, tc_color_freeze, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Use skin colors for frozen tees")
MACRO_CONFIG_INT(TcColorFreezeDarken, tc_color_freeze_darken, 90, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Makes color of tees darker when in freeze (0-100)")
MACRO_CONFIG_INT(TcColorFreezeFeet, tc_color_freeze_feet, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Also use color for frozen tee feet")

// Revert Variables
MACRO_CONFIG_INT(TcSmoothPredictionMargin, tc_prediction_margin_smooth, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Makes prediction margin transition smooth, causes worse ping jitter adjustment (reverts a DDNet change)")
MACRO_CONFIG_INT(TcFrozenKatana, tc_frozen_katana, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show katana on frozen players (reverts a DDNet change)")
MACRO_CONFIG_INT(TcOldTeamColors, tc_old_team_colors, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Use rainbow team colors (reverts a DDNet change)")
MACRO_CONFIG_INT(TcRevertHookLine, tc_revert_hook_line, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Revert hookline tip behavior")

// Outline Variables
MACRO_CONFIG_INT(TcOutline, tc_outline, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Draws outlines")
MACRO_CONFIG_INT(TcOutlineEntities, tc_outline_in_entities, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Only show outlines in entities")

MACRO_CONFIG_INT(TcOutlineSolid, tc_outline_solid, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Draws outline around hook and unhook")
MACRO_CONFIG_INT(TcOutlineFreeze, tc_outline_freeze, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Draws outline around freeze and deep")
MACRO_CONFIG_INT(TcOutlineUnfreeze, tc_outline_unfreeze, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Draws outline around unfreeze and undeep")
MACRO_CONFIG_INT(TcOutlineKill, tc_outline_kill, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Draws outline around kill")
MACRO_CONFIG_INT(TcOutlineTele, tc_outline_tele, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Draws outline around teleporters")

MACRO_CONFIG_INT(TcOutlineWidthSolid, tc_outline_width_solid, 2, 1, 16, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Width of outline around hook and unhook")
MACRO_CONFIG_INT(TcOutlineWidthFreeze, tc_outline_width_freeze, 2, 1, 16, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Width of outline around freeze and deep")
MACRO_CONFIG_INT(TcOutlineWidthUnfreeze, tc_outline_width_unfreeze, 2, 1, 16, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Width of outline around unfreeze and undeep")
MACRO_CONFIG_INT(TcOutlineWidthKill, tc_outline_width_kill, 2, 1, 16, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Width of outline around kill")
MACRO_CONFIG_INT(TcOutlineWidthTele, tc_outline_width_tele, 2, 1, 16, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Width of outline around teleporters")

MACRO_CONFIG_COL(TcOutlineColorSolid, tc_outline_color_solid, 4294901760, CFGFLAG_CLIENT | CFGFLAG_SAVE | CFGFLAG_COLALPHA, "Color of outline around hook and unhook") // 255 0 0 0
MACRO_CONFIG_COL(TcOutlineColorFreeze, tc_outline_color_freeze, 4294901760, CFGFLAG_CLIENT | CFGFLAG_SAVE | CFGFLAG_COLALPHA, "Color of outline around freeze and deep") // 255 0 0 0
MACRO_CONFIG_COL(TcOutlineColorUnfreeze, tc_outline_color_unfreeze, 4294901760, CFGFLAG_CLIENT | CFGFLAG_SAVE | CFGFLAG_COLALPHA, "Color of outline around unfreeze and undeep") // 255 0 0 0
MACRO_CONFIG_COL(TcOutlineColorKill, tc_outline_color_kill, 4294901760, CFGFLAG_CLIENT | CFGFLAG_SAVE | CFGFLAG_COLALPHA, "Color of outline around kill") // 0 0 0
MACRO_CONFIG_COL(TcOutlineColorTele, tc_outline_color_tele, 4294901760, CFGFLAG_CLIENT | CFGFLAG_SAVE | CFGFLAG_COLALPHA, "Color of outline around teleporters") // 255 0 0 0

// Indicator Variables
MACRO_CONFIG_COL(TcIndicatorAlive, tc_indicator_alive, 255, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Color of alive tees in player indicator")
MACRO_CONFIG_COL(TcIndicatorFreeze, tc_indicator_freeze, 65407, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Color of frozen tees in player indicator")
MACRO_CONFIG_COL(TcIndicatorSaved, tc_indicator_dead, 0, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Color of tees who is getting saved in player indicator")
MACRO_CONFIG_INT(TcIndicatorOffset, tc_indicator_offset, 42, 16, 200, CFGFLAG_CLIENT | CFGFLAG_SAVE, "(16-128) Offset of indicator position")
MACRO_CONFIG_INT(TcIndicatorOffsetMax, tc_indicator_offset_max, 100, 16, 200, CFGFLAG_CLIENT | CFGFLAG_SAVE, "(16-128) Max indicator offset for variable offset setting")
MACRO_CONFIG_INT(TcIndicatorVariableDistance, tc_indicator_variable_distance, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Indicator circles will be further away the further the tee is")
MACRO_CONFIG_INT(TcIndicatorMaxDistance, tc_indicator_variable_max_distance, 1000, 500, 7000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Maximum tee distance for variable offset")
MACRO_CONFIG_INT(TcIndicatorRadius, tc_indicator_radius, 4, 1, 16, CFGFLAG_CLIENT | CFGFLAG_SAVE, "(1-16) indicator circle size")
MACRO_CONFIG_INT(TcIndicatorOpacity, tc_indicator_opacity, 50, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Opacity of indicator circles")
MACRO_CONFIG_INT(TcPlayerIndicator, tc_player_indicator, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show radial indicator of other tees")
MACRO_CONFIG_INT(TcPlayerIndicatorFreeze, tc_player_indicator_freeze, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Only show frozen tees in indicator")
MACRO_CONFIG_INT(TcIndicatorTeamOnly, tc_indicator_inteam, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Only show indicator while in team")
MACRO_CONFIG_INT(TcIndicatorTees, tc_indicator_tees, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show tees instead of circles")
MACRO_CONFIG_INT(TcIndicatorHideVisible, tc_indicator_hide_visible_tees, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Don't show tees that are on your screen")

// Bind Wheel
MACRO_CONFIG_INT(TcResetBindWheelMouse, tc_reset_bindwheel_mouse, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Reset position of mouse when opening bindwheel")

// Regex chat matching
MACRO_CONFIG_STR(TcRegexChatIgnore, tc_regex_chat_ignore, 512, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Filters out chat messages based on a regular expression.")

// Misc visual
MACRO_CONFIG_INT(TcWhiteFeet, tc_white_feet, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Render all feet as perfectly white base color")
MACRO_CONFIG_STR(TcWhiteFeetSkin, tc_white_feet_skin, 255, "x_ninja", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Base skin for white feet")
MACRO_CONFIG_INT(TcRenderWeaponsAsGun, tc_render_weapons_as_gun, 0, 0, 2, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Renders weapons as the gun sprite instead of the weapon, with the exception of hammer and ninja (1 = with hue, 2 = without hue)")
MACRO_CONFIG_INT(TcMovingTilesEntities, tc_moving_tiles_entities, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show moving tiles in entities")

MACRO_CONFIG_INT(TcMiniDebug, tc_mini_debug, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show position and angle")

MACRO_CONFIG_INT(TcNotifyWhenLast, tc_last_notify, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Notify when you are last")
MACRO_CONFIG_STR(TcNotifyWhenLastText, tc_last_notify_text, 64, "Last!", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Text for last notify")
MACRO_CONFIG_COL(TcNotifyWhenLastColor, tc_last_notify_color, 256, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Color for last notify")
MACRO_CONFIG_INT(TcNotifyWhenLastX, tc_last_notify_x, 20, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Horizontal position for last notify as percentage of screen width")
MACRO_CONFIG_INT(TcNotifyWhenLastY, tc_last_notify_y, 1, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Vertical position for last notify as percentage of screen height")
MACRO_CONFIG_INT(TcNotifyWhenLastSize, tc_last_notify_size, 10, 0, 50, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Font size for last notify")

MACRO_CONFIG_INT(TcRenderCursorSpec, tc_cursor_in_spec, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Render your gun cursor when spectating in freeview")
MACRO_CONFIG_INT(TcRenderCursorSpecAlpha, tc_cursor_in_spec_alpha, 100, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Alpha of cursor in freeview")

// MACRO_CONFIG_INT(TcRenderNameplateSpec, tc_render_nameplate_spec, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Render nameplates when spectating")

MACRO_CONFIG_INT(TcTinyTees, tc_tiny_tees, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Render tees smaller")
MACRO_CONFIG_INT(TcTinyTeeSize, tc_indicator_tees_size, 100, 85, 115, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Define the Size of the Tiny Tee")
MACRO_CONFIG_INT(TcTinyTeesOthers, tc_tiny_tees_others, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Render other tees smaller")

MACRO_CONFIG_INT(TcCursorScale, tc_cursor_scale, 100, 0, 500, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Percentage to scale the in game cursor by as a percentage (50 = half, 200 = double)")

// Profiles
MACRO_CONFIG_INT(TcProfileSkin, tc_profile_skin, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Apply skin in profiles")
MACRO_CONFIG_INT(TcProfileName, tc_profile_name, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Apply name in profiles")
MACRO_CONFIG_INT(TcProfileClan, tc_profile_clan, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Apply clan in profiles")
MACRO_CONFIG_INT(TcProfileFlag, tc_profile_flag, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Apply flag in profiles")
MACRO_CONFIG_INT(TcProfileColors, tc_profile_colors, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Apply colors in profiles")
MACRO_CONFIG_INT(TcProfileEmote, tc_profile_emote, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Apply emote in profiles")
MACRO_CONFIG_INT(TcProfileOverwriteClanWithEmpty, tc_profile_overwrite_clan_with_empty, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Overwrite clan name even if profile has an empty clan name")

// Rainbow
MACRO_CONFIG_INT(TcRainbowTees, tc_rainbow_tees, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Turn on rainbow client side")
MACRO_CONFIG_INT(TcRainbowHook, tc_rainbow_hook, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Rainbow hook")
MACRO_CONFIG_INT(TcRainbowWeapon, tc_rainbow_weapon, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Rainbow Weapons")

MACRO_CONFIG_INT(TcRainbowOthers, tc_rainbow_others, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Turn on rainbow client side for others")
MACRO_CONFIG_INT(TcRainbowMode, tc_rainbow_mode, 1, 1, 4, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Rainbow mode (1: rainbow, 2: pulse, 3: darkness, 4: random)")
MACRO_CONFIG_INT(TcRainbowSpeed, tc_rainbow_speed, 100, 0, 10000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Rainbow speed as a percentage (50 = half speed, 200 = double speed)")

// War List
MACRO_CONFIG_INT(TcWarList, tc_warlist, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Toggles war list visuals")
MACRO_CONFIG_INT(TcWarListShowClan, tc_warlist_show_clan_if_war, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show clan in nameplate if there is a war")
MACRO_CONFIG_INT(TcWarListReason, tc_warlist_reason, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show war reason")
MACRO_CONFIG_INT(TcWarListChat, tc_warlist_chat, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show war colors in chat")
MACRO_CONFIG_INT(TcWarListScoreboard, tc_warlist_scoreboard, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show war colors in scoreboard")
MACRO_CONFIG_INT(TcWarListAllowDuplicates, tc_warlist_allow_duplicates, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Allow duplicate war entries")
MACRO_CONFIG_INT(TcWarListSpectate, tc_warlist_spectate, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show war colors in spectator menu")

MACRO_CONFIG_INT(TcWarListIndicator, tc_warlist_indicator, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Use warlist for indicator")
MACRO_CONFIG_INT(TcWarListIndicatorColors, tc_warlist_indicator_colors, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show warlist colors instead of freeze colors")
MACRO_CONFIG_INT(TcWarListIndicatorAll, tc_warlist_indicator_all, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show all groups")
MACRO_CONFIG_INT(TcWarListIndicatorEnemy, tc_warlist_indicator_enemy, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show players from the first group")
MACRO_CONFIG_INT(TcWarListIndicatorTeam, tc_warlist_indicator_team, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show players from second group")

// Status Bar
MACRO_CONFIG_INT(TcStatusBar, tc_statusbar, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Enable status bar")

MACRO_CONFIG_INT(TcStatusBar12HourClock, tc_statusbar_12_hour_clock, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Use 12 hour clock in local time")
MACRO_CONFIG_INT(TcStatusBarLocalTimeSeconds, tc_statusbar_local_time_seconds, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show seconds in local time")
MACRO_CONFIG_INT(TcStatusBarHeight, tc_statusbar_height, 8, 1, 16, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Height of the status bar")

MACRO_CONFIG_COL(TcStatusBarColor, tc_statusbar_color, 3221225472, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Status bar background color")
MACRO_CONFIG_COL(TcStatusBarTextColor, tc_statusbar_text_color, 4278190335, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Status bar text color")
MACRO_CONFIG_INT(TcStatusBarAlpha, tc_statusbar_alpha, 75, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Status bar background alpha")
MACRO_CONFIG_INT(TcStatusBarTextAlpha, tc_statusbar_text_alpha, 100, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Status bar text alpha")

MACRO_CONFIG_INT(TcStatusBarLabels, tc_statusbar_labels, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show labels on status bar entries")
MACRO_CONFIG_STR(TcStatusBarScheme, tc_statusbar_scheme, 128, "ac pf r", CFGFLAG_CLIENT | CFGFLAG_SAVE, "The order in which to show status bar items")

// Trails
MACRO_CONFIG_INT(TcTeeTrail, tc_tee_trail, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Enable Tee trails")
MACRO_CONFIG_INT(TcTeeTrailOthers, tc_tee_trail_others, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show tee trails for other players")
MACRO_CONFIG_INT(TcTeeTrailWidth, tc_tee_trail_width, 15, 0, 20, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Tee trail width")
MACRO_CONFIG_INT(TcTeeTrailLength, tc_tee_trail_length, 25, 5, 200, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Tee trail length")
MACRO_CONFIG_INT(TcTeeTrailAlpha, tc_tee_trail_alpha, 80, 1, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Tee trail alpha")
MACRO_CONFIG_COL(TcTeeTrailColor, tc_tee_trail_color, 255, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Tee trail color")
MACRO_CONFIG_INT(TcTeeTrailTaper, tc_tee_trail_taper, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Taper tee trail over length")
MACRO_CONFIG_INT(TcTeeTrailFade, tc_tee_trail_fade, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Fade trail alpha over length")
MACRO_CONFIG_INT(TcTeeTrailColorMode, tc_tee_trail_color_mode, 1, 1, 5, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Tee trail color mode (1: Solid color, 2: Current Tee color, 3: Rainbow, 4: Color based on Tee speed, 5: Random)")

// Chat Reply
MACRO_CONFIG_INT(TcAutoReplyMuted, tc_auto_reply_muted, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Auto reply to muted players with a message")
MACRO_CONFIG_STR(TcAutoReplyMutedMessage, tc_auto_reply_muted_message, 128, "I have muted you", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Message to reply to muted players")
MACRO_CONFIG_INT(TcAutoReplyMinimized, tc_auto_reply_minimized, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Auto reply when your game is minimized")
MACRO_CONFIG_STR(TcAutoReplyMinimizedMessage, tc_auto_reply_minimized_message, 128, "I am not tabbed in", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Message to reply when your game is minimized")

// Voting
MACRO_CONFIG_INT(TcAutoVoteWhenFar, tc_auto_vote_when_far, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Auto vote no if you far on a map")
MACRO_CONFIG_STR(TcAutoVoteWhenFarMessage, tc_auto_vote_when_far_message, 128, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Message to send when auto far vote happens, leave empty to disable")
MACRO_CONFIG_INT(TcAutoVoteWhenFarTime, tc_auto_vote_when_far_time, 5, 0, 20, CFGFLAG_CLIENT | CFGFLAG_SAVE, "How long until auto vote far happens")

// Font
MACRO_CONFIG_STR(TcCustomFont, tc_custom_font, 255, "DejaVu Sans", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Custom font face")

// Bg Draw
MACRO_CONFIG_INT(TcBgDrawWidth, tc_bg_draw_width, 5, 1, 50, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Width of background draw strokes")
MACRO_CONFIG_INT(TcBgDrawFadeTime, tc_bg_draw_fade_time, 0, 0, 600, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Time until strokes disappear (0 = never)")
MACRO_CONFIG_INT(TcBgDrawMaxItems, tc_bg_draw_max_items, 128, 0, 2048, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Maximum number of strokes")
MACRO_CONFIG_COL(TcBgDrawColor, tc_bg_draw_color, 14024576, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Color of background draw strokes")
MACRO_CONFIG_INT(TcBgDrawAutoSaveLoad, tc_bg_draw_auto_save_load, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Automatically save and load background drawings")

// Translate
MACRO_CONFIG_STR(TcTranslateBackend, tc_translate_backend, 32, "google", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Translate backend to use (google, libretranslate, tencent, baidu, deepl, mymemory, llm)")
MACRO_CONFIG_STR(TcTranslateTarget, tc_translate_target, 16, "en", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Translate target language (must be 2 character ISO 639 code)")
MACRO_CONFIG_STR(TcTranslateEndpoint, tc_translate_endpoint, 256, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "For backends which need it, endpoint to use (must be https)")
MACRO_CONFIG_STR(TcTranslateKey, tc_translate_key, 256, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "For backends which need it, api key to use")
MACRO_CONFIG_STR(TcTranslateTencentSecretId, tc_translate_tencent_secret_id, 128, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "腾讯云机器翻译 SecretId")
MACRO_CONFIG_STR(TcTranslateTencentSecretKey, tc_translate_tencent_secret_key, 128, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "腾讯云机器翻译 SecretKey")
MACRO_CONFIG_STR(TcTranslateTencentRegion, tc_translate_tencent_region, 32, "ap-guangzhou", CFGFLAG_CLIENT | CFGFLAG_SAVE, "腾讯云机器翻译地域（如 ap-guangzhou）")
MACRO_CONFIG_STR(TcTranslateBaiduAppId, tc_translate_baidu_appid, 64, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "百度翻译开放平台 APP ID")
MACRO_CONFIG_STR(TcTranslateBaiduKey, tc_translate_baidu_key, 128, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "百度翻译开放平台密钥")
MACRO_CONFIG_STR(TcTranslateDeeplKey, tc_translate_deepl_key, 128, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "DeepL API 密钥（免费版以 :fx 结尾）")
MACRO_CONFIG_STR(TcTranslateLlmEndpoint, tc_translate_llm_endpoint, 256, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "LLM 翻译端点（OpenAI 兼容，base URL 或完整 chat/completions 地址）")
MACRO_CONFIG_STR(TcTranslateLlmKey, tc_translate_llm_key, 256, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "LLM 翻译 API 密钥（本地端点可留空）")
MACRO_CONFIG_STR(TcTranslateLlmModel, tc_translate_llm_model, 128, "gpt-4o-mini", CFGFLAG_CLIENT | CFGFLAG_SAVE, "LLM 翻译使用的模型名")
MACRO_CONFIG_INT(TcTranslateAuto, tc_translate_auto, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Automatically translate incoming messages")
MACRO_CONFIG_INT(TcTranslateOutgoing, tc_translate_outgoing, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Translate your own outgoing chat messages into another language before sending them")
MACRO_CONFIG_STR(TcTranslateOutgoingTarget, tc_translate_outgoing_target, 16, "en", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Language to translate your outgoing messages into (must be 2 character ISO 639 code)")

// Animations
MACRO_CONFIG_INT(TcAnimateWheelTime, tc_animate_wheel_time, 80, 0, 1000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Duration of emote and bind wheel animations, in milliseconds (0 == no animation, 1000 = 1 second)")

// Pets
MACRO_CONFIG_INT(TcPetShow, tc_pet_show, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show a pet")
MACRO_CONFIG_STR(TcPetSkin, tc_pet_skin, 24, "twinbop", CFGFLAG_CLIENT | CFGFLAG_SAVE | CFGFLAG_INSENSITIVE, "Pet skin")
MACRO_CONFIG_INT(TcPetSize, tc_pet_size, 60, 10, 500, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Size of the pet as a percentage of a normal player")
MACRO_CONFIG_INT(TcPetAlpha, tc_pet_alpha, 90, 10, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Alpha of pet (100 = fully opaque, 50 = half transparent)")

// Change name near finish
MACRO_CONFIG_INT(TcChangeNameNearFinish, tc_change_name_near_finish, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Attempt to change your name when near finish")
MACRO_CONFIG_STR(TcFinishName, tc_finish_name, 16, "nameless tee", CFGFLAG_CLIENT | CFGFLAG_SAVE | CFGFLAG_INSENSITIVE, "Name to change to when near finish when tc_change_name_near_finish is 1")

// Flags
MACRO_CONFIG_INT(TcTClientSettingsTabs, tc_tclient_settings_tabs, 0, 0, 65536, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Bit flags to disable settings tabs")

// Volleyball
MACRO_CONFIG_INT(TcVolleyBallBetterBall, tc_volleyball_better_ball, 1, 0, 2, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Make frozen players in volleyball look more like volleyballs (0 = disabled, 1 = in volleyball maps, 2 = always)")
MACRO_CONFIG_STR(TcVolleyBallBetterBallSkin, tc_volleyball_better_ball_skin, 24, "Volleyball", CFGFLAG_CLIENT | CFGFLAG_SAVE | CFGFLAG_INSENSITIVE, "Player skin to use for better volleyball ball")

// Mod
MACRO_CONFIG_INT(TcShowPlayerHitBoxes, tc_show_player_hit_boxes, 0, 0, 2, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show player hit boxes (1 = predicted, 2 = predicted and unpredicted)")
MACRO_CONFIG_INT(TcHideChatBubbles, tc_hide_chat_bubbles, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Hide your own chat bubbles, only works when authed in remote console")
MACRO_CONFIG_INT(TcModWeapon, tc_mod_weapon, 0, 0, 1, CFGFLAG_CLIENT, "Run a command (default kill) when you point and shoot at someone, only works when authed in remote console")
MACRO_CONFIG_STR(TcModWeaponCommand, tc_mod_weapon_command, 256, "rcon kill_pl", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Command to run with tc_mod_weapon, id is appended to end of command")

// Run on join
MACRO_CONFIG_STR(TcExecuteOnConnect, tc_execute_on_connect, 100, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "")
MACRO_CONFIG_STR(TcExecuteOnJoin, tc_execute_on_join, 100, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "")
MACRO_CONFIG_INT(TcExecuteOnJoinDelay, tc_execute_on_join_delay, 7, 7, 50000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Tick delay before executing tc_execute_on_join")

// Custom Communities
MACRO_CONFIG_STR(TcCustomCommunitiesUrl, tc_custom_communities_url, 256, "https://raw.githubusercontent.com/TaterClient/ddnet-custom-communities/refs/heads/main/custom-communities-ddnet-info.json", CFGFLAG_CLIENT | CFGFLAG_SAVE, "URL to fetch custom communities from (must be https), empty to disable")

// Discord RPC
MACRO_CONFIG_INT(TcDiscordRPC, tc_discord_rpc, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Toggle discord RPC (requires restart)") // broken

MACRO_CONFIG_INT(TcShowLocalTimeSeconds, tc_show_local_time_seconds, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show local time in seconds")

// Configs tab UI
MACRO_CONFIG_INT(TcUiShowDDNet, tc_ui_show_ddnet, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show DDNet domain in Configs tab")
MACRO_CONFIG_INT(TcUiShowTClient, tc_ui_show_tclient, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show TClient domain in Configs tab")
MACRO_CONFIG_INT(TcUiOnlyModified, tc_ui_only_modified, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show only modified settings in Configs tab")
MACRO_CONFIG_INT(TcUiCompactList, tc_ui_compact_list, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Use compact row layout in Configs tab")

// Dummy Info
MACRO_CONFIG_INT(TcShowhudDummyPosition, tc_showhud_dummy_position, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show ingame HUD (Dummy Position)")
MACRO_CONFIG_INT(TcShowhudDummySpeed, tc_showhud_dummy_speed, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show ingame HUD (Dummy Speed)")
MACRO_CONFIG_INT(TcShowhudDummyAngle, tc_showhud_dummy_angle, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show ingame HUD (Dummy Aim Angle)")

// Dynamic Island (music player HUD)
MACRO_CONFIG_INT(TcIslandEnable, tc_island_enable, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island music HUD")
MACRO_CONFIG_INT(TcIslandNotch, tc_island_notch, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island notch style (attach to top edge)")
MACRO_CONFIG_INT(TcIslandShowLyrics, tc_island_show_lyrics, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island show synced lyrics")
MACRO_CONFIG_INT(TcIslandShowProgress, tc_island_show_progress, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island show game clock (round timer)")
MACRO_CONFIG_INT(TcIslandShowSongProgress, tc_island_show_song_progress, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island song progress rail at the bottom")
MACRO_CONFIG_STR(TcIslandFont, tc_island_font, 128, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island font face (empty = follow the global custom font)")
MACRO_CONFIG_INT(TcIslandSize, tc_island_size, 100, 60, 200, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island size (%)")
MACRO_CONFIG_INT(TcIslandNetease, tc_island_netease, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island read NetEase CloudMusic desktop via window title monitor")
MACRO_CONFIG_INT(TcIslandLyricOffset, tc_island_lyric_offset, 0, -30000, 30000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island lyric timing offset in ms (negative = earlier / ahead)")
MACRO_CONFIG_INT(TcIslandKaraokeLead, tc_island_karaoke_lead, 60, 0, 2000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island karaoke green fill lead in ms (how far the green wipe runs ahead of the lyric)")

// ME page: streamer mode (ported from QmClient)
MACRO_CONFIG_INT(MeStreamerMode, me_streamer_mode, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "ME streamer mode master switch")
MACRO_CONFIG_INT(MeStreamerFilterWords, me_streamer_filter_words, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Streamer mode: filter sensitive words")
MACRO_CONFIG_INT(MeStreamerHideNames, me_streamer_hide_names, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Streamer mode: replace non-friend names with client ID and hide clans")
MACRO_CONFIG_INT(MeStreamerHideSkins, me_streamer_hide_skins, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Streamer mode: non-friends use default skin")
MACRO_CONFIG_INT(MeStreamerDefaultFlags, me_streamer_default_flags, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Streamer mode: hide country flags everywhere (no flag is drawn)")
MACRO_CONFIG_STR(MeStreamerWordsFile, me_streamer_words_file, 1024, "", CFGFLAG_CLIENT | CFGFLAG_SAVE, "Streamer mode: extra word list file (one word per line)")

// ME page: per-category message muting
MACRO_CONFIG_INT(MeMuteTeamChat, me_mute_team_chat, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "ME: hide team chat messages")
MACRO_CONFIG_INT(MeMuteWhisper, me_mute_whisper, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "ME: hide whisper messages")
MACRO_CONFIG_INT(MeMuteSystemChat, me_mute_system_chat, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "ME: hide system/server chat lines")
MACRO_CONFIG_INT(MeMuteKillFeed, me_mute_kill_feed, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "ME: hide kill feed messages")
MACRO_CONFIG_INT(MeMuteFinishFeed, me_mute_finish_feed, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "ME: hide race finish messages")
MACRO_CONFIG_INT(TcIslandOffsetX, tc_island_offset_x, 0, -2000, 2000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island horizontal offset in pixels (at 100% size)")
MACRO_CONFIG_INT(TcIslandOffsetY, tc_island_offset_y, 0, -2000, 2000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island vertical offset in pixels (at 100% size)")
MACRO_CONFIG_INT(TcIslandTransparent, tc_island_transparent, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island transparent mode (no black capsule, outlined text)")
MACRO_CONFIG_INT(TcIslandTransparentTextShadow, tc_island_transparent_text_shadow, 85, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island transparent mode text outline strength (%)")
MACRO_CONFIG_INT(TcIslandShadow, tc_island_shadow, 100, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island drop shadow strength under the capsule (softens the hard edge against the game)")
// 胶囊配色：两个端点色构成线性渐变，方向由 tc_island_gradient 决定（0=纯色）。
// 默认上浅下深，是一条极轻的竖向渐变——观感与旧的近黑平色几乎一致，但多了层次。
MACRO_CONFIG_COL(TcIslandColorTop, tc_island_color_top, 10244371, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island capsule gradient start color (#0D1119)")
MACRO_CONFIG_COL(TcIslandColorBottom, tc_island_color_bottom, 10251527, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island capsule gradient end color (#04060A)")
MACRO_CONFIG_INT(TcIslandGradient, tc_island_gradient, 1, 0, 3, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island capsule gradient direction (0=flat, 1=vertical, 2=diagonal, 3=horizontal)")
// 强调色：卡拉OK已唱部分、底部进度轨、右侧等化器共用这一个颜色。
MACRO_CONFIG_COL(TcIslandColorAccent, tc_island_color_accent, 6602362, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Dynamic Island accent color for karaoke fill, progress rail and equalizer (#1FD661)")
MACRO_CONFIG_INT(TcPingwheelVolume, tc_pingwheel_volume, 100, 0, 100, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Ping wheel signal sound volume (%)")
MACRO_CONFIG_INT(TcPingwheelSoundRange, tc_pingwheel_sound_range, 600, 0, 10000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Ping wheel sound audible range in world units (0 = global, no falloff)")
MACRO_CONFIG_INT(TcPingwheelEmote, tc_pingwheel_emote, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Ping wheel fallback channel (no relay): 1 = emoticons (visible to all, only TClient decodes), 0 = local only")
MACRO_CONFIG_INT(TcRelay, tc_relay, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Ping wheel signal relay: send/receive pings via the TClient signal server (works while spectating, invisible to chat)")
MACRO_CONFIG_STR(TcRelayAddr, tc_relay_addr, 64, "8.163.48.23:8305", CFGFLAG_CLIENT | CFGFLAG_SAVE, "TClient signal server address (ip:port, udp)")
MACRO_CONFIG_INT(TcPingwheelEmoteMs, tc_pingwheel_emote_ms, 60, 20, 5000, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Emoticon channel send interval in ms (must be >= server sv_emoticon_ms_delay, else frames get dropped)")
MACRO_CONFIG_INT(TcTranslateRecentCount, tc_translate_recent_count, 5, 1, 16, CFGFLAG_CLIENT | CFGFLAG_SAVE, "How many recent non-Chinese chat lines the translate-recent bind translates")
MACRO_CONFIG_INT(TcScoreboardMedia, tc_scoreboard_media, 1, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show music controls on the left of the scoreboard")
