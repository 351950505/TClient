#!/usr/bin/env python3
"""Guard required release build options and regional probe arguments."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release_orchestrator import release_cmake_configure_command, release_mirror_probe_command


def main():
    args = release_cmake_configure_command("cmake.exe", "source", "build")
    assert args == ["cmake.exe", "-S", "source", "-B", "build", "-DTCLIENT_RELEASE_MIRRORS=ON", "-DVULKAN=ON"]

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
