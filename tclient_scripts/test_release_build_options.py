#!/usr/bin/env python3
"""Guard required release build options and regional probe arguments."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release_orchestrator import GH_EXE, release_cmake_build_command, release_cmake_configure_command, release_mirror_probe_command
from sync_master_manifest import GH as MASTER_SYNC_GH


def main():
    assert GH_EXE.is_file(), "release orchestration GitHub CLI path must resolve"
    assert MASTER_SYNC_GH.is_file(), "master manifest sync GitHub CLI path must resolve"

    args = release_cmake_configure_command("cmake.exe", "source", "build")
    assert args == ["cmake.exe", "-S", "source", "-B", "build", "-DTCLIENT_RELEASE_MIRRORS=ON", "-DVULKAN=ON"]

    build = release_cmake_build_command("cmake.exe", "build")
    assert build[build.index("--parallel") + 1] == "2", "limit build parallelism in the release workflow"
    assert "/p:CL_MPCount=1" in build[build.index("--") + 1:], "cap MSVC compiler children too"

    base_spec = {"version": "1.7.9.3", "target_commit": "a" * 40, "mirror_probe_nodes": []}
    args = release_mirror_probe_command("python", "probe.py", "repo", "report", "header", base_spec)
    assert args == ["python", "-I", "probe.py", "--repo", "repo", "--output", "report", "--header", "header",
                    "--release-version", "1.7.9.3", "--source-commit", "a" * 40], "local-only probe should not add remote nodes"

    regional_spec = {
        **base_spec,
        "mirror_probe_nodes": [
            {"region": "cn-guangzhou", "host": "aliyun"},
            {"region": "cn-beijing", "host": "beijing"},
        ],
    }
    args = release_mirror_probe_command("python", "probe.py", "repo", "report", "header", regional_spec)
    assert args[-4:] == ["--probe-node", "cn-guangzhou=aliyun", "--probe-node", "cn-beijing=beijing"]
    print("OK: release builds enable Vulkan and support local-only or explicit regional probes")


if __name__ == "__main__":
    main()
