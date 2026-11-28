"""Reviewed, opt-in local suites. Discovery alone never authorizes execution."""

SUITES = [
    {
        "id": "verifier-self", "type": "python", "path": "tclient_scripts/test_verify_tclient.py",
        "contract": "unittest", "groups": ["foundation"], "quick": True,
        "evidence": "调度器进程/输入/结果契约", "timeout": 120,
    },
    {
        "id": "selection-policy", "type": "python", "path": "tclient_scripts/test_verification_policy.py",
        "contract": "unittest", "groups": ["foundation"], "quick": True,
        "evidence": "纯选测策略", "timeout": 30,
    },
    {
        "id": "tooltip-wiring", "type": "node", "path": "scripts/test_checkbox_tooltip_wiring.js",
        "contract": "marker", "marker": r"^OK: checkbox hover help wiring verified \([1-9][0-9]* entries, Simplified Chinese complete\)$",
        "groups": ["translation", "ui"], "quick": True,
        "evidence": "源码/词典接线（非GUI）", "timeout": 30,
    },
    {
        "id": "map-progress-wiring", "type": "node", "path": "scripts/test_tclient_map_progress_wiring.js",
        "contract": "marker", "marker": r"^Map-progress default, budget, reset, HUD, settings, locale and test wiring verified\.$",
        "groups": ["ui", "runtime"], "quick": True,
        "evidence": "源码接线（非实际地图场景）", "timeout": 30,
    },
    {
        "id": "scoreboard-wiring", "type": "node", "path": "scripts/test_scoreboard_invite.js",
        "contract": "marker", "marker": r"^PASS: scoreboard invite uses the active local team,.*$",
        "groups": ["ui", "translation", "runtime"], "quick": True,
        "evidence": "源码接线（不发送消息）", "timeout": 30,
    },
    {
        "id": "release-offline", "type": "python", "path": "tclient_scripts/test_release_orchestrator.py",
        "args": ["DryRunTests", "ReleaseBodyEncodingTests", "MasterSyncParsingTests", "PublishTests"],
        "contract": "unittest", "groups": ["updater"], "quick": False,
        "evidence": "生产Python流程+模拟外部调用（指定子集）", "timeout": 120,
    },
    {
        "id": "stage-offline", "type": "python", "path": "tclient_scripts/test_prepare_release_stage.py",
        "args": ["PrepareReleaseStageTest.test_fresh_stage_uses_only_build_and_explicit_binary_sources",
                 "PrepareReleaseStageTest.test_shared_complete_ci_artifact_root_has_explicit_binary_origins",
                 "PrepareReleaseStageTest.test_cli_rejects_incomplete_inputs_and_inappropriate_verify_options"],
        "contract": "unittest", "groups": ["updater"], "quick": False,
        "evidence": "生产文件整理函数+临时文件（指定子集）", "timeout": 90,
    },
    {
        "id": "mirror-offline", "type": "python", "path": "tclient_scripts/test_probe_update_mirrors.py",
        "contract": "unittest", "groups": ["updater"], "quick": False,
        "requires_python": ["cryptography"],
        "evidence": "生产解析/验签+模拟网络（非当次镜像门禁）", "timeout": 120,
    },
    {
        "id": "build", "type": "build", "groups": ["build", "native"], "quick": False,
        "evidence": "本轮实际编译（不是测试用例）",
    },
    {
        "id": "native", "type": "native", "groups": ["native"], "quick": False,
        "evidence": "原生生产组件；不含完整客户端进程",
    },
    {
        "id": "rust", "type": "cmake-target", "target": "run_rust_tests",
        "contract": "cargo", "groups": ["rust"], "quick": False,
        "targets": {
            "ddnet_base:unit": 0, "ddnet_base:doc": 10,
            "ddnet_engine:unit": 0, "ddnet_engine:doc": 8,
            "ddnet_engine_shared:unit": 0, "ddnet_engine_shared:doc": 0,
            "ddnet_test:unit": 0, "ddnet_test:doc": 0,
        },
        "evidence": "当前候选依赖的离线 Cargo 测试；包含18个文档用例及6个零用例目标，不包含 mastersrv 实际服务场景",
        "timeout": 3600,
    },
    {
        "id": "translations", "type": "python", "path": "tclient_scripts/check_translations.py",
        "contract": "translations", "groups": ["translation", "ui"], "quick": True,
        "evidence": "全客户端 TCLocalize 词典检查；繁体缺词仅报告，不作为本地化阻断",
        "timeout": 60,
    },
    {
        "id": "fastinput-lifecycle", "type": "python", "path": "scripts/test_fastinput_lifecycle.py",
        "contract": "marker", "marker": r"^checks=([1-9][0-9]*) failures=0$",
        "marker_count_group": 1,
        "groups": ["input", "runtime"], "quick": True,
        "evidence": "输入源码约束检查（不是客户端操作场景）", "timeout": 30,
    },
    {
        "id": "quick-zoom-default", "type": "node", "path": "scripts/test_quick_zoom_restores_default.js",
        "contract": "marker", "marker": r"^Quick zoom toggle-on selects the quick target; toggle-off restores configured default\.$",
        "groups": ["input", "ui"], "quick": True,
        "evidence": "源码契约检查（不是鼠标实测）", "timeout": 30,
    },
    {
        "id": "freeze-entry-authority", "type": "node", "path": "scripts/test_freeze_entry_server_confirmation.js",
        "contract": "marker",
        "marker": r"^Freeze-entry episodes use authoritative freeze and thaw snapshots and do not reflash while frozen\.$",
        "groups": ["runtime", "input", "ui"], "quick": True,
        "evidence": "冻结状态/快照源码边界（非在线实测）", "timeout": 30,
    },
    {
        "id": "mirror-selection", "type": "python", "path": "tclient_scripts/test_mirror_selection.py",
        "contract": "marker",
        "marker": r"^OK: mirror selection keeps 2 global-edge slots, balances measured regions and preserves speed-order fallback$",
        "groups": ["updater"], "quick": True,
        "evidence": "镜像选择纯内存规则（不执行探测请求）", "timeout": 30,
    },
]

COVERAGE_GAPS = [
    "full 是本清单全量，不是整个项目所有脚本/玩法已覆盖；未登记脚本不自动执行。",
    "真实客户端、GUI、在线网络、播放器/手感仍需按改动实机验收；本工具不启动 DDNet。",
    "Rust 通过 run_rust_tests 接入；standalone mastersrv/服务进程场景仍未接入。",
    "release-offline 仅登记4个安全测试类；stage-offline 仅登记3个无链接权限要求的用例，报告不冒充完整套件。",
    "检查范围限于 TCLocalize 调用；其他翻译/资源文字未纳入，繁体缺词沿用项目原有非阻断提示。",
    "生产镜像测速、签名、整包/更新安装验收与发版接入属于后续阶段，不由本入口自动触发。",
]
