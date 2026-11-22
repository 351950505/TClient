#!/usr/bin/env python3
"""镜像选择的地区多样性回归（2026-11-22 用户要求：测速考虑其他地区玩家）。

锁定 probe_update_mirrors.select_mirrors 的行为：
- 选择集合里 Cloudflare（全球 anycast，海外可达）镜像至少保留 MIN_GLOBAL_SELECTED 个名额（池中够多少给多少）；
- 输出仍按本机测速降序（国内玩家的优先级不变）；
- 池中没有全球型镜像时退化为纯速度选择，不引入未验证镜像。

用 python -I 运行，退出码 0 = 全部通过。
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from probe_update_mirrors import MIN_GLOBAL_SELECTED, select_mirrors


def row(prefix, speed, edge):
    return {"prefix": prefix, "combined_bytes_per_second": speed, "edge": edge}


def main():
    max_selected = 5
    # 1) 混合池：速度前五全是区域镜像时，全球型必须被换入至少 2 个名额
    rows = [row("a-regional.example/", 100.0, "nginx"),
            row("b-regional.example/", 90.0, "nginx"),
            row("c-regional.example/", 80.0, "nginx"),
            row("d-regional.example/", 70.0, "nginx"),
            row("e-regional.example/", 60.0, "nginx"),
            row("f-cloudflare.example/", 50.0, "cloudflare"),
            row("g-cloudflare.example/", 40.0, "cloudflare"),
            row("h-cloudflare.example/", 30.0, "cloudflare")]
    selected = select_mirrors(rows, max_selected)
    assert len(selected) == max_selected, "should fill all slots"
    assert selected[0]["prefix"] == "a-regional.example/", "fastest mirror stays first regardless of edge"
    global_count = sum(1 for entry in selected if entry["edge"] == "cloudflare")
    assert global_count >= MIN_GLOBAL_SELECTED, f"global quota not met: {global_count}"
    speeds = [entry["combined_bytes_per_second"] for entry in selected]
    assert speeds == sorted(speeds, reverse=True), "output must stay speed-ordered"

    # 2) 全球型镜像本来就快时，它们自然入选，不需要挤掉任何人
    rows = [row("a-cloudflare.example/", 100.0, "cloudflare"),
            row("b-cloudflare.example/", 90.0, "cloudflare"),
            row("c-regional.example/", 80.0, "nginx"),
            row("d-regional.example/", 70.0, "nginx"),
            row("e-regional.example/", 60.0, "nginx"),
            row("f-regional.example/", 50.0, "nginx")]
    selected = select_mirrors(rows, max_selected)
    assert [entry["prefix"] for entry in selected] == ["a-cloudflare.example/", "b-cloudflare.example/",
                                                       "c-regional.example/", "d-regional.example/",
                                                       "e-regional.example/"], "fast pool keeps pure speed order"

    # 3) 池中没有全球型镜像：退化为纯速度选择，不凑数
    rows = [row(f"r{i}.example/", 100.0 - i, "nginx") for i in range(7)]
    selected = select_mirrors(rows, max_selected)
    assert len(selected) == max_selected, "degrades to speed selection"
    assert all(entry["edge"] == "nginx" for entry in selected), "no invented mirrors"

    # 4) 全球型只有一个：尽力保留它，不强制凑满配额
    rows = [row("r0.example/", 100.0, "nginx"),
            row("r1.example/", 90.0, "nginx"),
            row("one-cloudflare.example/", 80.0, "cloudflare"),
            row("r2.example/", 70.0, "nginx"),
            row("r3.example/", 60.0, "nginx"),
            row("r4.example/", 50.0, "nginx")]
    selected = select_mirrors(rows, max_selected)
    assert "one-cloudflare.example/" in [entry["prefix"] for entry in selected], "lone global mirror is kept"

    # 5) 名额不足/边界：单条、恰好 min_global、零池
    single = select_mirrors([row("cf.example/", 10.0, "cloudflare")], 1)
    assert [entry["prefix"] for entry in single] == ["cf.example/"], "single mirror round-trips"
    exact = select_mirrors([row("c1.example/", 20.0, "cloudflare"), row("c2.example/", 10.0, "cloudflare"),
                            row("r0.example/", 30.0, "nginx")], 3)
    assert len(exact) == 3 and sum(1 for entry in exact if entry["edge"] == "cloudflare") == 2, "small pool respects quota"
    assert select_mirrors([], 5) == [], "empty pool yields nothing"

    # 6) 速度并列时按 prefix 稳定排序（两次调用结果一致）
    tied = [row("t2.example/", 50.0, "cloudflare"), row("t1.example/", 50.0, "cloudflare")]
    assert [entry["prefix"] for entry in select_mirrors(tied, 2)] == ["t1.example/", "t2.example/"], "tie broken by prefix"

    print(f"OK: mirror selection keeps {MIN_GLOBAL_SELECTED} global-edge slots, speed order and degeneration verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
