#!/usr/bin/env python3
"""检查 TClient 的 TCLocalize 词条是否都有简体中文翻译。

v1.7.8.62 发布后玩家反馈 ME 页 Steam 区块显示英文原文。根因不是渲染或编码，
而是源码里的 `TCLocalize("...")` 在 `data/tclient/languages/simplified_chinese.txt`
里根本没有对应条目——漏翻译在编译、testrunner 和发布链上都看不出来。

因此把检查前移到发布前：新增词条必须同时补词典，否则 preflight 失败。

用法:
    python check_translations.py [--repo <repo_root>]
    python check_translations.py --list          # 只列出缺失项

退出码: 0 = 全部已有翻译, 1 = 有缺失
"""
import argparse
import re
import sys
from collections import Counter
from pathlib import Path

LANG_RELPATH = Path("data/tclient/languages/simplified_chinese.txt")
SCAN_RELDIRS = ("src/game/client/components/tclient", "src/game/localization.cpp")

# TCLocalize("...") / TCLocalize("...", "...")，与 scripts/languages/tclient 保持同一形态
KEY_RE = re.compile(r'TCLocalize\s*\(\s*"((?:(?:\\")|[^"])*)"\s*(?:,\s*"((?:(?:\\")|[^"])*)"\s*)?\)')

BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")

# 宏形式的 context 参数（如 DATA_VERSION_PATH）；按源码里的字面值展开。
CONTEXT_MACROS = {"DATA_VERSION_PATH": "data_version.txt"}


def strip_comments(text):
    """去掉注释，避免把注释掉的 TCLocalize 当成真词条。

    只用最朴素的两种正则；TClient 源码里没有字符串内出现 ``//`` 或 ``/*`` 的
    TCLocalize key，因此不需要完整的 C 词法分析。
    """
    text = BLOCK_COMMENT_RE.sub("", text)
    return LINE_COMMENT_RE.sub("", text)


def collect_keys(repo):
    """收集源码里所有 TCLocalize 的 (key, context)，按出现次数计数。

    context 与 key 必须配对比较：``TCLocalize("%s not found", DATA_VERSION_PATH)``
    查的是 ``("%s not found", "data_version.txt")``，同名 key 但 context 不同不算命中。
    """
    keys = Counter()
    scanned = 0
    for rel in SCAN_RELDIRS:
        target = repo / rel
        if target.is_file():
            paths = [target]
        elif target.is_dir():
            paths = sorted(list(target.glob("*.cpp")) + list(target.glob("*.h")))
        else:
            continue
        for path in paths:
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                text = path.read_text(encoding="utf-8", errors="replace")
            if "TCLocalize" not in text:
                continue
            scanned += 1
            for match in KEY_RE.finditer(strip_comments(text)):
                key = match.group(1).replace('\\"', '"')
                raw_context = (match.group(2) or "").replace('\\"', '"')
                keys[(key, CONTEXT_MACROS.get(raw_context, raw_context))] += 1
    return keys, scanned


def lookup_hit(entry, translated):
    """模拟 ``CLocalizationDatabase::FindString`` 的 context 回退。

    带 context 查不到时会退到默认 context 再查一次（localization.cpp:270-278）；
    反向不会回退，所以只存在于 ``[某 context]`` 段的 key 不能命中无 context 的调用。
    """
    key, context = entry
    if entry in translated:
        return True
    return bool(context) and (key, "") in translated


def collect_translated(lang_path):
    """解析词典已有的 (key, context) 对。

    词典格式：可选的 ``[context]`` 行，其后是英文行与 ``== 译文`` 行；
    空行和 ``#`` 注释行结束当前 context（与 ``twlang_tclient.decode`` 一致）。
    """
    translated = set()
    context = ""
    lines = lang_path.read_text(encoding="utf-8").splitlines()
    for index, line in enumerate(lines):
        if not line or line.startswith("#"):
            context = ""
            continue
        if line.startswith("[") and not line.startswith("[%") and line.endswith("]"):
            context = line[1:-1]
            continue
        if line.startswith("== "):
            continue
        if index + 1 < len(lines) and lines[index + 1].startswith("== "):
            translated.add((line, context))
    return translated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent), help="repository root")
    parser.add_argument("--list", action="store_true", help="only list missing keys")
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    lang_path = repo / LANG_RELPATH
    if not lang_path.is_file():
        print(f"FAIL: language file not found: {lang_path}", file=sys.stderr)
        return 1

    keys, scanned = collect_keys(repo)
    translated = collect_translated(lang_path)
    missing = sorted(entry for entry in keys if not lookup_hit(entry, translated))

    def label(entry):
        key, context = entry
        return f"{key}  [context: {context}]" if context else key

    if args.list:
        for entry in missing:
            print(label(entry))
        return 0 if not missing else 1

    print(f"scanned {scanned} files, {len(keys)} keys, {len(keys) - len(missing)} translated, {len(missing)} missing")
    if missing:
        print("missing translations in " + str(LANG_RELPATH) + ":")
        for entry in missing:
            print(f"  [{keys[entry]}x] {label(entry)}")
        print("\nAdd each key to the language file as:")
        print("  [context]            (only when the C++ call passes a context)")
        print("  <english key>")
        print("  == <chinese translation>")
        return 1
    print("OK: every TCLocalize key has a simplified Chinese translation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
