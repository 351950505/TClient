#!/usr/bin/env python3
"""Prepare a fresh Windows release stage without borrowing old executable bytes.

Run with ``python -I prepare_release_stage.py --help``. Schema version 1 records
absolute input roots and per-file ``path``, ``origin``, ``role``, ``size`` and
``sha256``. The external report is a local provenance snapshot, not a signature
or a replacement for the release pipeline's Authenticode verification.

``validate_stage(stage, report, allow_first_party_changes=True)`` permits later
signing of ONLY DDNet.exe, TClientUpdater.exe and island_media_monitor.exe.
``validate_files(stage, inventory, require_stage_root=False, ...)`` also checks
an extracted archive without reading the original build/resource directories.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import shutil
import stat
import sys


SCHEMA_VERSION = 1
FIRST_PARTY_FILES = frozenset(("DDNet.exe", "TClientUpdater.exe", "island_media_monitor.exe"))
REQUIRED_BUILD_FILES = ("DDNet.exe", "TClientUpdater.exe", "steam_api.dll")
HELPERS = {
	"monitor": ("island_media_monitor.exe", "first_party"),
	"watcher": ("netease-watcher.exe", "vendor"),
	"hook_dll": ("wndhok.dll", "vendor"),
}
REQUIRED_STAGE_FILES = frozenset(REQUIRED_BUILD_FILES) | frozenset(name for name, _role in HELPERS.values())
ROOT_RESOURCE_NAMES = frozenset((
	"readme", "readme.txt", "readme.md", "license", "license.txt", "license.md",
	"copying", "copying.txt", "autoexec.cfg", "steam_appid.txt",
	"ddnet.ico", "ddnet.png", "tclient.ico", "tclient.png",
))
# Filter every depth of a resource tree, including disguised backup names.
CODE_SUFFIXES = frozenset((
	".exe", ".dll", ".pdb", ".com", ".scr", ".sys", ".ocx", ".cpl", ".msi", ".msp",
	".bat", ".cmd", ".ps1", ".psm1", ".psd1", ".py", ".pyw", ".js", ".jse",
	".vbs", ".vbe", ".wsf", ".wsh", ".sh", ".so", ".dylib", ".a", ".lib", ".obj", ".o",
))
EXCLUDED_RESOURCE_DIRS = frozenset((
	"screenshots", "demos", "downloadedmaps", "downloads", "update", "updates",
	"updater", "crashlogs", "logs", "__pycache__", ".git",
))
ROOT_KEYS = frozenset(("build_dir", "runtime_dirs", "resources", "data_dir", "monitor", "watcher", "hook_dll"))
REPARSE_POINT = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)


class StageError(ValueError):
	"""An invalid stage, unsafe path, changed file, or filesystem failure."""


def _absolute(path, label):
	try:
		text = os.fspath(path)
		if not isinstance(text, str) or not text or "\0" in text:
			raise StageError(f"{label}: expected a nonempty path")
		if ".." in PureWindowsPath(text).parts or ".." in Path(text).parts:
			raise StageError(f"{label}: path traversal is forbidden: {text}")
		return Path(os.path.abspath(text))
	except (TypeError, ValueError) as error:
		if isinstance(error, StageError):
			raise
		raise StageError(f"{label}: invalid path: {error}") from error


def _is_link(info):
	return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & REPARSE_POINT)


def _no_links(path):
	for item in (*reversed(path.parents), path):
		try:
			info = item.lstat()
		except FileNotFoundError:
			continue
		if _is_link(info):
			raise StageError(f"symlink, junction or reparse point is forbidden: {item}")


def _directory(path, label):
	_no_links(path)
	try:
		info = path.lstat()
	except FileNotFoundError as error:
		raise StageError(f"{label} directory missing: {path}") from error
	if not stat.S_ISDIR(info.st_mode):
		raise StageError(f"{label} is not a directory: {path}")


def _file(path, label, *, nonempty=False):
	_no_links(path)
	try:
		info = path.lstat()
	except FileNotFoundError as error:
		raise StageError(f"{label} file missing: {path}") from error
	if not stat.S_ISREG(info.st_mode):
		raise StageError(f"{label} is not a regular file: {path}")
	if nonempty and info.st_size == 0:
		raise StageError(f"{label} file is empty: {path}")
	return info


def _new_path(path, label):
	_no_links(path)
	try:
		path.lstat()
	except FileNotFoundError:
		pass
	else:
		raise StageError(f"{label} must not already exist: {path}")
	_directory(path.parent, f"{label} parent")


def _within(path, root):
	return path == root or root in path.parents


def _overlaps(left, right):
	return _within(left, right) or _within(right, left)


def _relative(text):
	if not isinstance(text, str) or not text or "\\" in text or ":" in text:
		raise StageError(f"invalid relative inventory path: {text!r}")
	parts = text.split("/")
	if any(not part or part in (".", "..") or part.endswith((" ", ".")) or any(ord(c) < 32 for c in part) for part in parts):
		raise StageError(f"unsafe relative inventory path: {text!r}")
	if PurePosixPath(text).is_absolute() or PureWindowsPath(text).drive:
		raise StageError(f"absolute inventory path is forbidden: {text!r}")
	# Keep Windows device names and alternative data streams out on every OS.
	for part in parts:
		stem = part.split(".", 1)[0].casefold()
		if stem in {"con", "prn", "aux", "nul"} or re.fullmatch(r"(?:com|lpt)[1-9]", stem) or any(c in '<>"|?*' for c in part):
			raise StageError(f"unsafe Windows inventory path: {text!r}")
	return text


def _excluded_resource_part(name):
	lower = name.casefold()
	return (
		lower in EXCLUDED_RESOURCE_DIRS or lower.endswith((".log", ".dmp"))
		or ".bak" in lower or ".old_" in lower
		or lower.startswith(("update_backup", "update_staging", "update_tmp", ".update", "tclientupdater"))
	)


def _resource_name_allowed(relative):
	parts = relative.split("/")
	if any(_excluded_resource_part(part) for part in parts):
		return False
	name = parts[-1].casefold()
	user_settings = name.startswith("settings") and name.endswith((".cfg", ".ini", ".json", ".dat", ".state"))
	if user_settings or name.startswith("crashlog") or name.endswith((".tmp", ".log", ".dmp")) or ".bak" in name or ".old_" in name:
		return False
	if any(suffix.casefold() in CODE_SUFFIXES for suffix in PurePosixPath(name).suffixes):
		return False
	return (len(parts) == 1 and name in ROOT_RESOURCE_NAMES) or (len(parts) > 1 and parts[0] == "data")


def _executable_magic(path):
	with path.open("rb") as handle:
		header = handle.read(4)
	return header.startswith((b"MZ", b"#!", b"\x7fELF")) or header in (
		b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe",
	)


def _walk(root, *, prune_resources=False):
	"""Walk regular files without following links, including Windows junctions."""
	_directory(root, "input")
	def fail_walk(error):
		raise error
	for current, dirs, names in os.walk(root, followlinks=False, onerror=fail_walk):
		current = Path(current)
		dirs.sort()
		for name in dirs + sorted(names):
			path = current / name
			info = path.lstat()
			if _is_link(info):
				raise StageError(f"symlink, junction or reparse point is forbidden: {path}")
			if not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
				raise StageError(f"non-regular resource entry is forbidden: {path}")
		# Validate names before filtering, never follow a pruned link.
		for name in dirs + sorted(names):
			_relative((current / name).relative_to(root).as_posix())
		if prune_resources:
			dirs[:] = [name for name in dirs if not _excluded_resource_part(name)]
		for name in sorted(names):
			yield current / name


def _fingerprint(path):
	before = _file(path, "source")
	digest = hashlib.sha256()
	size = 0
	with path.open("rb") as handle:
		for chunk in iter(lambda: handle.read(1024 * 1024), b""):
			digest.update(chunk)
			size += len(chunk)
	after = _file(path, "source")
	if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns) or size != after.st_size:
		raise StageError(f"source changed while reading: {path}")
	return size, digest.hexdigest()


def _copy_file(source, destination, record):
	_file(source, "source", nonempty=record["role"] != "resource")
	destination.parent.mkdir(parents=True, exist_ok=True)
	_no_links(destination.parent)
	digest = hashlib.sha256()
	size = 0
	with source.open("rb") as src, destination.open("xb") as dst:
		for chunk in iter(lambda: src.read(1024 * 1024), b""):
			dst.write(chunk)
			digest.update(chunk)
			size += len(chunk)
	_file(source, "source")
	if size != record["size"] or digest.hexdigest() != record["sha256"]:
		raise StageError(f"source changed while copying: {source}")


def _metadata_path(value, label):
	if not isinstance(value, str) or not Path(value).is_absolute():
		raise StageError(f"inventory {label} must be an absolute path")
	return _absolute(value, f"inventory {label}")


def _validate_inventory(inventory):
	if not isinstance(inventory, dict) or type(inventory.get("schema_version")) is not int or inventory["schema_version"] != SCHEMA_VERSION:
		raise StageError("unsupported inventory schema_version (expected 1)")
	stage_root = _metadata_path(inventory.get("stage_root"), "stage_root")
	stamp = inventory.get("created_utc")
	try:
		if not isinstance(stamp, str) or not stamp.endswith("Z"):
			raise ValueError("UTC timestamp required")
		datetime.fromisoformat(stamp[:-1] + "+00:00")
	except ValueError as error:
		raise StageError("inventory created_utc must be an ISO UTC timestamp") from error
	roots = inventory.get("roots")
	if not isinstance(roots, dict) or set(roots) != ROOT_KEYS:
		raise StageError("inventory roots must contain build_dir, runtime_dirs, resources, data_dir, monitor, watcher, hook_dll")
	if not isinstance(roots["runtime_dirs"], list):
		raise StageError("inventory runtime_dirs must be an array of absolute paths")
	runtime_roots = [_metadata_path(value, "runtime_dirs") for value in roots["runtime_dirs"]]
	roots = {
		name: (None if value is None and name in ("resources", "data_dir") else _metadata_path(value, name))
		for name, value in roots.items() if name != "runtime_dirs"
	}
	files = inventory.get("files")
	if not isinstance(files, list):
		raise StageError("inventory files must be an array")
	result = {}
	folded = set()
	for entry in files:
		if not isinstance(entry, dict) or set(entry) != {"path", "origin", "role", "size", "sha256"}:
			raise StageError("inventory file must contain path, origin, role, size and sha256")
		relative = _relative(entry["path"])
		if relative.casefold() in folded:
			raise StageError(f"duplicate or case-colliding inventory path: {relative}")
		folded.add(relative.casefold())
		if type(entry["size"]) is not int or entry["size"] < 0 or not isinstance(entry["sha256"], str) or re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) is None:
			raise StageError(f"invalid inventory size or sha256: {relative}")
		origin = _metadata_path(entry["origin"], f"origin for {relative}")
		if relative in FIRST_PARTY_FILES:
			role = "first_party"
			expected_origins = [roots["monitor"] if relative == "island_media_monitor.exe" else roots["build_dir"] / relative]
		elif relative in ("netease-watcher.exe", "wndhok.dll"):
			role = "vendor"
			expected_origins = [roots["watcher"] if relative == "netease-watcher.exe" else roots["hook_dll"]]
		elif "/" not in relative and relative.casefold().endswith(".dll"):
			role = "vendor"
			expected_origins = [root / relative for root in (roots["build_dir"], *runtime_roots)]
		elif _resource_name_allowed(relative):
			role = "resource"
			expected_origins = []
			if roots["resources"] is not None:
				expected_origins.append(roots["resources"] / relative)
			if roots["data_dir"] is not None and relative.startswith("data/"):
				expected_origins.append(roots["data_dir"] / relative[5:])
		else:
			raise StageError(f"file is outside stage allowlist: {relative}")
		if entry["role"] != role:
			raise StageError(f"inventory role mismatch for {relative}: expected {role}")
		if role != "resource" and entry["size"] == 0:
			raise StageError(f"inventory executable/library is empty: {relative}")
		if origin not in expected_origins:
			raise StageError(f"inventory origin does not match declared roots: {relative}")
		result[relative] = entry
	missing = REQUIRED_STAGE_FILES - result.keys()
	if missing:
		raise StageError(f"required stage files missing from inventory: {', '.join(sorted(missing))}")
	# A file cannot also be the parent directory of a second entry.
	for relative in result:
		for parent in PurePosixPath(relative).parents:
			if parent.as_posix().casefold() in folded:
				raise StageError(f"inventory file/directory conflict: {relative}")
	return stage_root, result


def validate_files(stage_dir, inventory, *, allow_first_party_changes=False, require_stage_root=True):
	"""Validate schema, exact file set and pinned bytes without reading origins.

	The signing allowance applies only to the three fixed FIRST_PARTY_FILES, and
	still requires nonempty regular files. Vendor files (including watcher/hook)
	and resources always retain the inventory's exact whole-file bytes.
	"""
	try:
		stage = _absolute(stage_dir, "stage")
		_directory(stage, "stage")
		stage_root, records = _validate_inventory(inventory)
		if require_stage_root and stage != stage_root:
			raise StageError(f"stage_root mismatch: inventory records {stage_root}, requested {stage}")
		actual = {path.relative_to(stage).as_posix(): path for path in _walk(stage)}
		missing = records.keys() - actual.keys()
		extra = actual.keys() - records.keys()
		if missing or extra:
			raise StageError(f"stage inventory file set mismatch; missing={sorted(missing)}, unexpected={sorted(extra)}")
		for relative, record in records.items():
			path = actual[relative]
			_file(path, "stage", nonempty=record["role"] != "resource")
			if allow_first_party_changes and record["role"] == "first_party":
				continue
			size, digest = _fingerprint(path)
			if size != record["size"]:
				raise StageError(f"stage size mismatch: {relative}")
			if digest != record["sha256"]:
				raise StageError(f"stage sha256 mismatch: {relative}")
			if record["role"] == "resource" and _executable_magic(path):
				raise StageError(f"executable bytes are forbidden in resources: {relative}")
		return inventory
	except OSError as error:
		raise StageError(f"could not validate stage: {error}") from error


def _unique_object(pairs):
	result = {}
	for key, value in pairs:
		if key in result:
			raise StageError(f"duplicate inventory JSON field: {key}")
		result[key] = value
	return result


def validate_stage(stage_dir, report_path, *, allow_first_party_changes=False):
	"""Read an external inventory and validate a stage; never write or sign."""
	try:
		stage = _absolute(stage_dir, "stage")
		report = _absolute(report_path, "report")
		if _overlaps(stage, report):
			raise StageError("report must be external to the stage")
		_file(report, "report", nonempty=True)
		try:
			inventory = json.loads(report.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
		except (UnicodeError, json.JSONDecodeError) as error:
			raise StageError(f"invalid inventory JSON: {error}") from error
		return validate_files(stage, inventory, allow_first_party_changes=allow_first_party_changes)
	except OSError as error:
		raise StageError(f"could not read stage report: {error}") from error


def _remove_created(path, identity, *, directory):
	try:
		info = path.lstat()
	except FileNotFoundError:
		return
	if _is_link(info) or (info.st_dev, info.st_ino) != identity:
		raise StageError(f"cleanup refused because created output was replaced: {path}")
	if directory:
		shutil.rmtree(path)
	else:
		path.unlink()


def prepare_stage(build_dir, output, report, *, monitor, watcher, hook_dll, resources=None, data_dir=None, runtime_dirs=()):
	"""Create a new stage and a new external schema-v1 report or remove both.

	All destination parent directories must already exist. Explicit helper flags
	map to canonical output names; no external directory is copied wholesale.
	Source data overlays the allowed resources/data tree. No executable, DLL,
	PDB, script, or executable-magic file is borrowed from either resource tree.
	"""
	created_stage = None
	created_report = None
	stage = report_path = None
	try:
		stage = _absolute(output, "output")
		report_path = _absolute(report, "report")
		roots = {"build_dir": _absolute(build_dir, "build-dir"), "resources": None, "data_dir": None}
		if isinstance(runtime_dirs, (str, bytes, os.PathLike)):
			raise StageError("runtime_dirs must be a sequence of explicit directory paths")
		runtime_roots = [_absolute(path, "runtime-dir") for path in runtime_dirs]
		for name, value in (("resources", resources), ("data_dir", data_dir), ("monitor", monitor), ("watcher", watcher), ("hook_dll", hook_dll)):
			if value is not None:
				roots[name] = _absolute(value, name)
			elif name in HELPERS:
				raise StageError(f"explicit --{name.replace('_', '-')} input is required")
		_new_path(stage, "output")
		_new_path(report_path, "report")
		if _overlaps(stage, report_path):
			raise StageError("report must be external to the stage")
		for name, path in [*roots.items(), *(("runtime_dir", path) for path in runtime_roots)]:
			if path is None:
				continue
			if _overlaps(stage, path):
				raise StageError(f"output overlaps {name} source: {path}")
			if _overlaps(report_path, path):
				raise StageError(f"report overlaps {name} source: {path}")
			if name in HELPERS:
				_file(path, name, nonempty=True)
			else:
				_directory(path, name)

		planned = {}
		def add(relative, origin, role, *, overlay=False, identical_vendor=False):
			_relative(relative)
			key = relative.casefold()
			_file(origin, "source", nonempty=role != "resource")
			if role == "resource" and _executable_magic(origin):
				return
			size, digest = _fingerprint(origin)
			if key in planned:
				previous = planned[key]
				if identical_vendor and previous["role"] == role == "vendor":
					if previous["size"] != size or previous["sha256"] != digest:
						raise StageError(f"conflicting vendor DLL bytes for {relative}: {previous['origin']} and {origin}")
					return
				if not (overlay and previous["path"] == relative and previous["role"] == role == "resource"):
					raise StageError(f"duplicate or case-colliding stage path: {relative}")
			planned[key] = {"path": relative, "origin": str(origin), "role": role, "size": size, "sha256": digest}

		build = roots["build_dir"]
		for name in REQUIRED_BUILD_FILES:
			_file(build / name, "required build", nonempty=True)
		for name in REQUIRED_BUILD_FILES[:2]:
			add(name, build / name, "first_party")
		for name, (destination, role) in HELPERS.items():
			add(destination, roots[name], role)
		for root in (build, *runtime_roots):
			for path in sorted(root.iterdir(), key=lambda path: path.name):
				if path.name.casefold().endswith(".dll"):
					add(path.name, path, "vendor", identical_vendor=True)

		if roots["resources"] is not None:
			resource_root = roots["resources"]
			for path in sorted(resource_root.iterdir(), key=lambda path: path.name):
				if path.name.casefold() in ROOT_RESOURCE_NAMES:
					add(path.name, path, "resource")
			resource_data = resource_root / "data"
			if os.path.lexists(resource_data):
				for path in _walk(resource_data, prune_resources=True):
					relative = "data/" + path.relative_to(resource_data).as_posix()
					if _resource_name_allowed(relative):
						add(relative, path, "resource")
		if roots["data_dir"] is not None:
			for path in _walk(roots["data_dir"], prune_resources=True):
				relative = "data/" + path.relative_to(roots["data_dir"]).as_posix()
				if _resource_name_allowed(relative):
					add(relative, path, "resource", overlay=True)

		inventory = {
			"schema_version": SCHEMA_VERSION,
			"stage_root": str(stage),
			"created_utc": datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
			"roots": {**{name: str(path) if path is not None else None for name, path in roots.items()}, "runtime_dirs": [str(path) for path in runtime_roots]},
			"files": sorted(planned.values(), key=lambda record: record["path"]),
		}
		_validate_inventory(inventory)
		stage.mkdir()
		info = stage.lstat()
		created_stage = (info.st_dev, info.st_ino)
		for record in inventory["files"]:
			_copy_file(Path(record["origin"]), stage / record["path"], record)
		validate_files(stage, inventory)
		with report_path.open("x", encoding="utf-8", newline="\n") as handle:
			info = os.fstat(handle.fileno())
			created_report = (info.st_dev, info.st_ino)
			json.dump(inventory, handle, indent=2, ensure_ascii=False)
			handle.write("\n")
		return inventory
	except BaseException as error:
		cleanup_errors = []
		for path, identity, directory in ((report_path, created_report, False), (stage, created_stage, True)):
			if identity is not None:
				try:
					_remove_created(path, identity, directory=directory)
				except (OSError, StageError) as cleanup_error:
					cleanup_errors.append(str(cleanup_error))
		if cleanup_errors:
			raise StageError(f"{error}; cleanup failed: {'; '.join(cleanup_errors)}") from error
		if isinstance(error, OSError):
			raise StageError(f"could not prepare stage: {error}") from error
		raise


def main(argv=None):
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	mode = parser.add_mutually_exclusive_group(required=True)
	mode.add_argument("--build-dir", help="fresh Release directory with DDNet.exe, TClientUpdater.exe and vendor DLLs")
	mode.add_argument("--verify-stage", help="read-only verification of an existing stage")
	parser.add_argument("--output", help="new, nonexistent stage directory")
	parser.add_argument("--report", required=True, help="external provenance JSON (must be new when preparing)")
	parser.add_argument("--resources", help="optional previous stage, used only for allowed game resources")
	parser.add_argument("--data-dir", help="optional source data directory, overlaid on data/")
	parser.add_argument("--runtime-dir", action="append", default=[], help="explicit runtime directory; copy only top-level DLLs (repeatable)")
	parser.add_argument("--monitor", help="explicit first-party island_media_monitor.exe input")
	parser.add_argument("--watcher", help="explicit original vendor netease-watcher.exe input")
	parser.add_argument("--hook-dll", help="explicit original vendor wndhok.dll input")
	parser.add_argument("--allow-first-party-changes", action="store_true", help="verification only: allow later first-party signing; all vendor/resource hashes remain pinned")
	args = parser.parse_args(argv)
	try:
		if args.verify_stage:
			if any((args.output, args.resources, args.data_dir, args.runtime_dir, args.monitor, args.watcher, args.hook_dll)):
				raise StageError("--verify-stage does not accept staging inputs or --output")
			inventory = validate_stage(args.verify_stage, args.report, allow_first_party_changes=args.allow_first_party_changes)
			print(f"Verified {len(inventory['files'])} staged files against {args.report}")
		else:
			if not args.output or not all((args.monitor, args.watcher, args.hook_dll)):
				raise StageError("staging requires --output, --monitor, --watcher and --hook-dll")
			if args.allow_first_party_changes:
				raise StageError("--allow-first-party-changes requires --verify-stage")
			inventory = prepare_stage(args.build_dir, args.output, args.report, resources=args.resources, data_dir=args.data_dir, runtime_dirs=args.runtime_dir, monitor=args.monitor, watcher=args.watcher, hook_dll=args.hook_dll)
			print(f"Prepared {len(inventory['files'])} files in {args.output}; provenance: {args.report}")
		return 0
	except StageError as error:
		print(f"error: {error}", file=sys.stderr)
		return 1


if __name__ == "__main__":
	sys.exit(main())
