#!/usr/bin/env python3
"""Create the TClient autoupdate release assets.

Given a packaged client directory (the contents of the portable client, i.e. the directory that
holds DDNet.exe, the DLLs and data/), this writes two files into the output directory:

  TClient-update-win64.zip    only the files an update needs to replace
  TClient-update-win64.json   manifest describing that zip (version, size, sha256, notes)

The zip is written deterministically: entries are sorted by name and all timestamps are fixed, so
building the same input twice produces byte identical output. Only the Python standard library is
used.

User data is never put into the zip: cfg/, demos/, screenshots/, themes/, downloadedmaps/,
tclient/ (custom fonts and other user resources), storage.cfg, settings_*.cfg, logs, crash dumps
and leftovers of previous updates are excluded.

Example:
  python tclient_scripts/make_release_assets.py \\
    --source "E:/TClient-10.9-win64" \\
    --version 1.4.0 \\
    --output "E:/TClient-release" \\
    --notes "Fix skin loading, add new pings"
"""

import argparse
import hashlib
import json
import os
import sys
import zipfile
from pathlib import Path

ASSET_NAME = "TClient-update-win64.zip"
MANIFEST_NAME = "TClient-update-win64.json"

# Fixed timestamp for every entry (the earliest value a zip file can store) so that the archive
# is reproducible.
FIXED_DATE_TIME = (1980, 1, 1, 0, 0, 0)

# Directories at the root of the installation that hold user data and must never be replaced.
EXCLUDED_ROOT_DIRS = (
	"cfg",
	"demos",
	"downloadedmaps",
	"screenshots",
	"tclient",
	"themes",
	"update",
)

# Files at the root of the installation that hold user data.
EXCLUDED_ROOT_FILES = ("storage.cfg",)

# Glob-style suffixes that are always excluded, wherever they appear.
EXCLUDED_SUFFIXES = (".log", ".dmp")

# Substrings that mark a file as deployment debris, e.g. the
# "DDNet.exe.bak-relay-20261030-203958" backups a manual deploy leaves behind.
# Shipping those would bloat the update by hundreds of megabytes.
EXCLUDED_NAME_PARTS = (".bak",)

# Prefixes of directories the updater creates itself.
EXCLUDED_DIR_PREFIXES = ("update_backup_", "update_staging_")

# Files that must be part of an update, otherwise the package is rejected as obviously broken.
REQUIRED_FILES = ("DDNet.exe",)


class PackageError(Exception):
	pass


def is_excluded(relative: Path) -> bool:
	"""True if the file must not end up in the update zip."""
	parts = relative.parts
	if not parts:
		return True

	name = parts[-1].lower()
	if name.endswith(EXCLUDED_SUFFIXES):
		return True
	if any(part in name for part in EXCLUDED_NAME_PARTS):
		return True

	if len(parts) == 1:
		if name in EXCLUDED_ROOT_FILES:
			return True
		if name.startswith("settings_") and name.endswith(".cfg"):
			return True

	root = parts[0].lower()
	if root in EXCLUDED_ROOT_DIRS:
		return True
	if any(root.startswith(prefix) for prefix in EXCLUDED_DIR_PREFIXES):
		return True

	# Nested leftovers of the updater, e.g. data/update_staging_1234/.
	for part in parts[:-1]:
		if part.lower().startswith(EXCLUDED_DIR_PREFIXES):
			return True

	return False


def collect_files(source: Path) -> list[tuple[str, Path]]:
	"""Returns (relative posix path, absolute path) for every file that belongs into the zip."""
	if not source.is_dir():
		raise PackageError(f"source directory does not exist: {source}")

	result = []
	for directory, dir_names, file_names in os.walk(source):
		dir_names.sort()
		# Prune excluded directories early so we do not walk huge user data trees.
		current = Path(directory)
		dir_names[:] = [
			name
			for name in dir_names
			if not is_excluded((current / name).relative_to(source))
		]
		for file_name in sorted(file_names):
			absolute = current / file_name
			relative = absolute.relative_to(source)
			if is_excluded(relative):
				continue
			result.append((relative.as_posix(), absolute))
	result.sort(key=lambda item: item[0])
	return result


def sha256_of(path: Path) -> str:
	digest = hashlib.sha256()
	with open(path, "rb") as file:
		while True:
			block = file.read(1024 * 1024)
			if not block:
				break
			digest.update(block)
	return digest.hexdigest()


def write_zip(files: list[tuple[str, Path]], destination: Path) -> None:
	temporary = destination.with_suffix(destination.suffix + ".tmp")
	if temporary.exists():
		temporary.unlink()
	with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
		for relative, absolute in files:
			info = zipfile.ZipInfo(relative, date_time=FIXED_DATE_TIME)
			info.compress_type = zipfile.ZIP_DEFLATED
			info.create_system = 0  # MS-DOS, so no unix permission bits end up in the archive
			info.external_attr = 0x20  # FILE_ATTRIBUTE_ARCHIVE
			with open(absolute, "rb") as source_file:
				archive.writestr(info, source_file.read())
	if destination.exists():
		destination.unlink()
	temporary.replace(destination)


def human_size(size: int) -> str:
	for unit in ("B", "KiB", "MiB", "GiB"):
		if size < 1024 or unit == "GiB":
			return f"{size:.0f} {unit}" if unit == "B" else f"{size:.2f} {unit}"
		size /= 1024
	return f"{size:.2f} GiB"


def load_notes(args: argparse.Namespace) -> str:
	if args.notes is not None:
		return args.notes
	if args.notes_file is not None:
		try:
			return Path(args.notes_file).read_text(encoding="utf-8").strip()
		except OSError as error:
			raise PackageError(f"could not read notes file: {error}") from error
	changelog = Path(args.source) / "CHANGELOG.md"
	if changelog.is_file():
		lines = changelog.read_text(encoding="utf-8", errors="replace").splitlines()
		section = []
		for line in lines:
			if line.startswith("#"):
				if section:
					break
				continue
			if line.strip():
				section.append(line.strip())
			if len(section) >= 10:
				break
		if section:
			return " ".join(section)
	return f"TClient {args.version}"


def parse_arguments(argv: list[str]) -> argparse.Namespace:
	parser = argparse.ArgumentParser(
		description="Create TClient-update-win64.zip and its json manifest.",
		formatter_class=argparse.RawTextHelpFormatter,
	)
	parser.add_argument("--source", required=True, help="Packaged client directory (contains DDNet.exe and data/)")
	parser.add_argument("--version", required=True, help="Client version, e.g. 1.4.0")
	parser.add_argument("--output", required=True, help="Directory the two assets are written to")
	parser.add_argument("--notes", default=None, help="Release notes stored in the manifest")
	parser.add_argument("--notes-file", default=None, help="Read the release notes from this file instead")
	parser.add_argument("--product", default="TClient", help="Product name in the manifest (default: TClient)")
	parser.add_argument("--channel", default="stable", help="Update channel in the manifest (default: stable)")
	parser.add_argument("--platform", default="win64", help="Platform in the manifest (default: win64)")
	parser.add_argument("--quiet", action="store_true", help="Only print the manifest and the summary")
	return parser.parse_args(argv)


def main(argv: list[str]) -> int:
	args = parse_arguments(argv)
	source = Path(args.source).resolve()
	output = Path(args.output).resolve()

	try:
		files = collect_files(source)
		if not files:
			raise PackageError(f"no files to package found in {source}")
		missing = [name for name in REQUIRED_FILES if name not in {relative for relative, _ in files}]
		if missing:
			raise PackageError(f"required files missing from the package: {', '.join(missing)}")

		notes = load_notes(args)
		output.mkdir(parents=True, exist_ok=True)
		zip_path = output / ASSET_NAME
		manifest_path = output / MANIFEST_NAME

		write_zip(files, zip_path)

		size = zip_path.stat().st_size
		digest = sha256_of(zip_path)
		uncompressed = sum(absolute.stat().st_size for _, absolute in files)

		manifest = {
			"product": args.product,
			"channel": args.channel,
			"version": args.version,
			"platform": args.platform,
			"asset": ASSET_NAME,
			"size": size,
			"sha256": digest,
			"notes": notes,
		}
		manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

		if not args.quiet:
			print(f"source: {source}")
			print(f"files in the update: {len(files)}")
			for relative, absolute in files:
				print(f"  {relative} ({human_size(absolute.stat().st_size)})")
		print()
		print(f"zip:      {zip_path}")
		print(f"manifest: {manifest_path}")
		print(f"files:    {len(files)}")
		print(f"raw size: {human_size(uncompressed)}")
		print(f"zip size: {human_size(size)} ({size} bytes)")
		print(f"sha256:   {digest}")
		print()
		print("manifest:")
		print(json.dumps(manifest, indent=2, ensure_ascii=False))
		return 0
	except PackageError as error:
		print(f"error: {error}", file=sys.stderr)
		return 1
	except OSError as error:
		print(f"error: {error}", file=sys.stderr)
		return 1


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))
