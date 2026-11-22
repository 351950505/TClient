#!/usr/bin/env python3
"""Guard required release build options and regional probe arguments."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release_orchestrator import release_cmake_configure_command, release_mirror_probe_command


def main():
    args = release_cmake_configure_command("cmake.exe", "source", "build")
    assert args == ["cmake.exe", "-S", "source", "-B", "build", "-DTCLIENT_RELEASE_MIRRORS=ON", "-DVULKAN=ON"]

    spec = {
        "version": "1.7.9.3",
        "target_commit": "a" * 40,
        "mirror_probe_nodes": [
            {"region": "cn-guangzhou", "host": "aliyun"},
            {"region": "cn-beijing", "host": "beijing"},
        ],
    }
    args = release_mirror_probe_command("python", "probe.py", "repo", "report", "header", spec)
    assert args[-4:] == ["--probe-node", "cn-guangzhou=aliyun", "--probe-node", "cn-beijing=beijing"]
    print("OK: release builds enable Vulkan and pass all regional mirror probes")


if __name__ == "__main__":
    main()
