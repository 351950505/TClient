"""Pure test selection: this module never reads files or runs test suites."""


_FEATURES = (
    (("translat", "localiz", "language"), "translation"),
    (("input", "bind", "control", "gamecore", "predict", "collision",
      "physics", "joystick", "touch", "mouse", "keyboard"), "input"),
    (("music", "lyric", "sound", "audio"), "music"),
    (("updat", "download", "http"), "updater"),
)
_MANUAL = (
    ("translation", "翻译：检查语言切换、缺失文本、乱码与文字截断。"),
    ("ui", "UI：使用真实配置检查布局、缩放、可见性与交互。"),
    ("input", "输入：实测按键、鼠标、重绑定、焦点切换及长按/松开，确认无卡键。"),
    ("runtime", "网络：实测连接、断线重连、地图下载与进入游戏，检查运行稳定性。"),
    ("music", "音乐：实测播放/暂停、切曲、音量、歌词与断网恢复。"),
    ("updater", "更新：实测下载、断点续传、校验、安装与失败恢复。"),
)


def _safe_path(path):
    """Accept unambiguous repository-relative POSIX paths, without normalization."""
    if not isinstance(path, str) or not path or not path.isprintable():
        return False
    if any(char in path for char in '\\:*?"<>|%{}[]'):
        return False
    return all(part and part not in (".", "..") and part == part.strip()
               and not part.endswith(".") for part in path.split("/"))


def _path_impact(path):
    if not _safe_path(path):
        return set(), "异常路径未映射：" + repr(path) + "；仅运行基础相关测试并提示人工核对范围。"
    path = path.lower()
    parts = path.split("/")
    name = parts[-1]
    if (name.endswith(".rs") or name in ("cargo.toml", "cargo.lock", "rust-toolchain", "rust-toolchain.toml")
            or path.startswith((".cargo/", "src/rust-bridge/", "src/masterping/", "src/mastersrv/"))):
        return {"native", "build", "runtime", "rust"}, "Rust 代码/依赖变更：限于相关 Rust、native/build/runtime 测试；正式发布前另跑 full：" + path
    if path.startswith(("src/base/", "src/engine/", "src/game/shared/",
                        "src/game/generated/", "src/game/gamecore",
                        "src/game/client/components/controls")):
        return {"native", "build", "runtime"}, "公共 core/输入变更：限于相关 native/build/runtime 测试；正式发布前另跑 full：" + path
    if (path.startswith(("deps/", ".cargo/")) or parts[0].startswith("cmake")
            or any(part in ("cmake", "toolchain", "toolchains") for part in parts[:-1])
            or name in ("cargo.toml", "cargo.lock", "rust-toolchain", "rust-toolchain.toml")
            or name.startswith(("cmakelists.txt", "cmakepresets.", "cmakeuserpresets."))
            or name.endswith(".cmake")
            or (name.startswith("requirements") and name.endswith(".txt"))):
        return {"native", "build", "runtime"}, "构建/依赖变更：限于相关 native/build/runtime 测试；正式发布前另跑 full：" + path
    if path.startswith(("scripts/", "tclient_scripts/", ".github/")):
        groups = set()
        for tokens, group in _FEATURES:
            if any(token in path for token in tokens):
                groups.add(group)
        if "tooltip" in path or "language" in path or "localiz" in path:
            groups.update(("translation", "ui"))
        if "map_progress" in path or "hud" in path:
            groups.update(("ui", "runtime"))
        if any(token in path for token in ("release_orchestrator", "prepare_release_stage",
                                           "make_release_assets", "probe_update_mirrors")):
            groups.add("updater")
        if path.startswith(("tclient_scripts/verification_", "tclient_scripts/test_verification_",
                            "tclient_scripts/verify_tclient.py", "tclient_scripts/test_verify_tclient.py")):
            groups.add("foundation")
        why = "验证工具/测试接线变更：选择基础与匹配的测试组。"
        if not groups:
            why += "人工确认映射范围。"
        return groups, why
    if path.startswith("data/tclient/languages/"):
        return {"translation", "ui"}, ""
    if path == "readme.md" or (path.startswith("docs/") and
                               path.endswith((".md", ".rst", ".txt"))):
        return set(), ""
    if path.startswith(("src/game/", "src/test/")):
        groups = {"native", "build", "runtime"}
        if path.startswith(("src/game/client/", "src/test/")):
            groups.add("ui")
        for tokens, group in _FEATURES:
            if any(token in path for token in tokens):
                groups.update((group, "ui"))
        return groups, ""
    return set(), "未知路径未映射：" + path + "；仅运行基础相关测试并提示人工核对范围。"


def plan_tests(mode, changed_paths, suites, *, trusted_baseline=False,
               baseline_reason='') -> dict:
    """Return a verification plan, not execution results or delivery evidence.

    The caller supplies both old and new paths for renames, and old paths for
    deletions. No file needs to exist. Only an explicit ``full`` mode selects
    the whole catalog; a missing/untrusted baseline adds a warning but never
    widens a plan/affected run. Suites retain catalog order, and only their id,
    groups and quick fields participate in the decision.
    """
    if mode not in ("plan", "quick", "affected", "full"):
        raise ValueError("mode must be plan, quick, affected or full")
    groups, path_reasons = set(), []
    for path in changed_paths:
        impact, why = _path_impact(path)
        groups.update(impact)
        if why:
            path_reasons.append(why)
    path_reasons = list(dict.fromkeys(path_reasons))

    full = mode == "full"
    warnings = list(path_reasons)
    if not trusted_baseline and mode not in ("quick", "full"):
        baseline_warning = "无可信 full 基线：按本次可观察到的路径做有限选测，本次结果不构成 full 通过证明。"
        if baseline_reason:
            baseline_warning += " 基线说明：" + baseline_reason
        warnings.insert(0, baseline_warning)
    reason = "full 模式要求全量验证。" if full else "；".join(warnings)
    escalation = reason
    if mode == "quick":
        escalation = "quick 仅提供快速反馈，不作交付证明。" + ("；" + reason if reason else "")
    elif mode == "plan":
        escalation = "plan 仅预览 affected 选测结果，不执行测试。" + ("；" + reason if reason else "")

    selected, not_selected = [], []
    for suite in suites:
        suite_groups = set(suite["groups"])
        matching = groups & suite_groups
        if full:
            destination, why = selected, reason
        elif "foundation" in suite_groups:
            destination, why = selected, "foundation：每次必选。"
        elif suite_groups & {"native", "build"} and not groups & {"native", "build"}:
            destination, why = not_selected, "native/build 仅在相关代码、构建变更或全量验证时选测。"
        elif mode != "quick" and matching:
            destination, why = selected, "改动涉及：" + "、".join(sorted(matching)) + "。"
        elif mode == "quick" and suite["quick"]:
            destination, why = selected, "quick：受限模式的快速检查，不作交付证明。"
        else:
            destination, why = not_selected, (
                "quick 模式未选此项；未选择不代表已通过。" if mode == "quick" else
                "未命中改动分组，且非 foundation/quick 检查；未选择不代表已通过。")
        destination.append({"id": suite["id"], "reason": why})
    manual = [text for group, text in _MANUAL if full or group in groups]
    if not trusted_baseline and mode not in ("quick", "full"):
        manual.append("测试范围：无可信 full 基线；本次为最小定向测试，不代表完整回归。")
    if any("人工确认" in warning or "未映射" in warning for warning in path_reasons):
        manual.append("测试范围：存在未映射或需特别关注的路径，人工确认以上定向测试覆盖了本次改动。")
    return {
        "full": full,
        "selected": selected,
        "not_selected": not_selected,
        "manual": manual,
        "escalation": escalation,
    }
