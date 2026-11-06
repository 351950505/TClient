#!/usr/bin/env python3
"""Create the TClient autoupdate release assets.

Given a packaged client directory (the contents of the portable client, i.e. the directory that
holds DDNet.exe, the DLLs and data/), this writes four release assets:

  TClient-update-win64.zip       only the files an update needs to replace
  TClient-update-win64.json      manifest describing that zip and the helper
  TClient-update-win64.json.sig  detached Ed25519 signature (64 raw bytes)
  TClientUpdater.exe             standalone bootstrap helper from the package

The zip is written deterministically: entries are sorted by name and all timestamps are fixed, so
building the same input twice produces byte identical output. Signing requires cryptography and an
Ed25519 PEM key matching the public key embedded in the client.

User data is never put into the zip: cfg/, demos/, screenshots/, themes/, downloadedmaps/,
tclient/ (custom fonts and other user resources), storage.cfg, settings_*.cfg, logs, crash dumps
and leftovers of previous updates are excluded.

Example (packaging also exports the authenticated pair to the exact raw directory):
  python -I tclient_scripts/make_release_assets.py \\
    --source "D:/build/client" \\
    --version 1.7.8.54 \\
    --output "D:/build/release-assets" \\
    --private-key "D:/secure/release-signing.pem" \\
    --raw-manifest-dir "D:/TaterClient-src/update" \\
    --notes "Fix skin loading, add new pings"

Read-only gate for an existing release (no signing material needed):
  python -I tclient_scripts/make_release_assets.py --verify \\
    --output "D:/build/release-assets" --version 1.7.8.54 \\
    --raw-manifest-dir "D:/TaterClient-src/update"
Add --raw-manifest-url https://raw.githubusercontent.com/OWNER/REPO/BRANCH/update/TClient-update-win64.json
only to also check the deployed pair. --verify never publishes or repairs a mismatch.
"""

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

ASSET_NAME = "TClient-update-win64.zip"
MANIFEST_NAME = "TClient-update-win64.json"
MANIFEST_SIG_NAME = MANIFEST_NAME + ".sig"
HELPER_NAME = "TClientUpdater.exe"
PUBLIC_KEY_HEADER = Path(__file__).resolve().parents[1] / "src/game/client/components/tclient/update_public_key.h"

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

# Subdirectories of data/ that the *update* package ships. Everything else under
# data/ is upstream game content: players customize some of it in place (sound
# packs in data/audio, custom skins in data/skins, own maps in data/maps), so an
# update that rewrites those files destroys their work. Only the resources this
# project maintains travel with an update; fresh installs use the full package.
UPDATE_DATA_SUBDIRS = ("tclient",)

# Files at the root of the installation that hold user data.
EXCLUDED_ROOT_FILES = ("storage.cfg",)

# Glob-style suffixes that are always excluded, wherever they appear.
EXCLUDED_SUFFIXES = (".log", ".dmp")

# Substrings that mark a file as deployment debris, e.g. the
# "DDNet.exe.bak-relay-20261030-203958" backups a manual deploy leaves behind,
# or the "TClientUpdater.exe.old_20261031_040654" renames the updater itself
# makes while self-replacing. Shipping those bloats the update.
EXCLUDED_NAME_PARTS = (".bak", ".old_")

# Prefixes of directories the updater creates itself.
EXCLUDED_DIR_PREFIXES = ("update_backup_", "update_staging_")

# Files that must be part of an update, otherwise the package is rejected as obviously broken.
REQUIRED_FILES = ("DDNet.exe",)


class PackageError(Exception):
	pass


def is_excluded(relative: Path, data_subdirs: tuple[str, ...] | None) -> bool:
	"""True if the file must not end up in the zip.

	data_subdirs selects how data/ is treated: None keeps every game resource
	(full installation package), a tuple keeps only the named subdirectories
	(update package), and an empty tuple drops data/ entirely.
	"""
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

	# data/ is upstream game content unless a subdirectory is named explicitly.
	if root == "data" and data_subdirs is not None and len(parts) >= 2 and parts[1].lower() not in data_subdirs:
		return True

	# Nested leftovers of the updater, e.g. data/update_staging_1234/.
	for part in parts[:-1]:
		if part.lower().startswith(EXCLUDED_DIR_PREFIXES):
			return True

	return False


def collect_files(source: Path, data_subdirs: tuple[str, ...]) -> list[tuple[str, Path]]:
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
			if not is_excluded((current / name).relative_to(source), data_subdirs)
		]
		for file_name in sorted(file_names):
			absolute = current / file_name
			relative = absolute.relative_to(source)
			if is_excluded(relative, data_subdirs):
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


def embedded_public_key_bytes() -> bytes:
	header = PUBLIC_KEY_HEADER.read_text(encoding="utf-8")
	match = re.search(r"s_aUpdateEd25519PublicKey\[32\]\s*=\s*\{([^}]+)\}", header)
	public_bytes = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})\b", match[1])) if match else b""
	if len(public_bytes) != 32:
		raise PackageError("could not read the client's embedded update public key")
	return public_bytes


def load_signing_key(path):
	if not path:
		raise PackageError("release signing requires --private-key (CI secret: TCLIENT_UPDATE_ED25519_PRIVATE_KEY)")
	try:
		from cryptography.hazmat.primitives import serialization
		from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
	except ImportError as error:
		raise PackageError("release signing requires the Python cryptography package") from error
	try:
		private_key = serialization.load_pem_private_key(Path(path).read_bytes(), password=None)
	except (ValueError, TypeError) as error:
		raise PackageError("release signing key must be an unencrypted Ed25519 PEM") from error
	if not isinstance(private_key, Ed25519PrivateKey):
		raise PackageError("release signing key is not an Ed25519 key")
	# Check the same trust anchor the client uses before producing any assets.
	if private_key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw) != embedded_public_key_bytes():
		raise PackageError("release signing key does not match the client's embedded update public key")
	return private_key


def validate_manifest(manifest_bytes: bytes, signature: bytes, expected_metadata: dict[str, str]) -> dict:
	"""Authenticate the original bytes, then enforce the release metadata contract."""
	try:
		from cryptography.exceptions import InvalidSignature
		from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
	except ImportError as error:
		raise PackageError("release verification requires the Python cryptography package") from error
	if len(signature) != 64:
		raise PackageError("manifest signature must contain exactly 64 raw bytes")
	try:
		Ed25519PublicKey.from_public_bytes(embedded_public_key_bytes()).verify(signature, manifest_bytes)
	except InvalidSignature as error:
		raise PackageError("manifest signature verification failed") from error

	def unique_object(pairs):
		result = {}
		for key, value in pairs:
			if key in result:
				raise PackageError(f"manifest contains duplicate field: {key}")
			result[key] = value
		return result

	try:
		manifest = json.loads(manifest_bytes.decode("utf-8"), object_pairs_hook=unique_object)
	except (UnicodeError, ValueError) as error:
		raise PackageError("manifest is not valid UTF-8 JSON") from error
	if not isinstance(manifest, dict):
		raise PackageError("manifest must be a JSON object")
	for field, expected in {**expected_metadata, "asset": ASSET_NAME}.items():
		if not expected or manifest.get(field) != expected:
			raise PackageError(f"manifest {field} mismatch: expected {expected!r}, got {manifest.get(field)!r}")
	if not isinstance(manifest.get("notes"), str):
		raise PackageError("manifest notes must be a string")
	helper = manifest.get("helper")
	if not isinstance(helper, dict) or helper.get("name") != HELPER_NAME:
		raise PackageError(f"manifest helper name must be {HELPER_NAME}")
	for name, metadata in [(ASSET_NAME, manifest), (HELPER_NAME, helper)]:
		if type(metadata.get("size")) is not int or metadata["size"] <= 0:
			raise PackageError(f"manifest {name} size must be a positive integer")
		if not isinstance(metadata.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", metadata["sha256"]):
			raise PackageError(f"manifest {name} sha256 must be 64 lowercase hex digits")
	return manifest


def verify_release_assets(output: Path, expected_metadata: dict[str, str]) -> tuple[bytes, bytes]:
	"""Verify existing local assets without signing, packaging or writing anything."""
	manifest_bytes = (output / MANIFEST_NAME).read_bytes()
	signature = (output / MANIFEST_SIG_NAME).read_bytes()
	manifest = validate_manifest(manifest_bytes, signature, expected_metadata)
	for name, metadata in [(ASSET_NAME, manifest), (HELPER_NAME, manifest["helper"])]:
		path = output / name
		if not path.is_file():
			raise PackageError(f"release asset missing: {path}")
		if path.stat().st_size != metadata["size"]:
			raise PackageError(f"release asset size mismatch: {path}")
		if sha256_of(path) != metadata["sha256"]:
			raise PackageError(f"release asset sha256 mismatch: {path}")
	return manifest_bytes, signature


def verify_matching_pair(pair: tuple[bytes, bytes], expected_pair: tuple[bytes, bytes], expected_metadata: dict[str, str], source: str) -> None:
	try:
		validate_manifest(*pair, expected_metadata)
	except PackageError as error:
		raise PackageError(f"{source}: {error}") from error
	if pair != expected_pair:
		raise PackageError(f"{source}: signed manifest bytes differ from release assets")


def read_manifest_pair(directory: Path) -> tuple[bytes, bytes]:
	return (directory / MANIFEST_NAME).read_bytes(), (directory / MANIFEST_SIG_NAME).read_bytes()


def read_remote_manifest_pair(url: str) -> tuple[bytes, bytes]:
	"""Read only the two deployed manifest files; never fetch or execute release assets."""
	try:
		parsed = urllib.parse.urlsplit(url)
	except ValueError as error:
		raise PackageError("--raw-manifest-url is not a valid HTTPS URL") from error
	if parsed.scheme != "https" or not parsed.netloc or parsed.username or parsed.password or parsed.query or parsed.fragment or not parsed.path.endswith("/" + MANIFEST_NAME):
		raise PackageError(f"--raw-manifest-url must be an HTTPS URL ending in /{MANIFEST_NAME}, without credentials, query or fragment")
	pair = []
	for address, limit in [(url, 1024 * 1024), (url + ".sig", 64)]:
		try:
			request = urllib.request.Request(address, headers={"Cache-Control": "no-cache"}, method="GET")
			with urllib.request.urlopen(request, timeout=30) as response:
				if urllib.parse.urlsplit(response.geturl()).scheme != "https":
					raise PackageError("raw manifest request redirected away from HTTPS")
				content = response.read(limit + 1)
		except (OSError, urllib.error.URLError) as error:
			raise PackageError(f"could not read deployed raw manifest: {error}") from error
		if len(content) > limit:
			raise PackageError(f"deployed raw manifest file exceeds {limit} bytes")
		pair.append(content)
	return pair[0], pair[1]


def write_manifest_pair(directory: Path, pair: tuple[bytes, bytes]) -> None:
	"""Stage both files before replacing either. An interrupted pair fails verification."""
	directory.mkdir(parents=True, exist_ok=True)
	temporary_paths = []
	try:
		for name, content in zip((MANIFEST_NAME, MANIFEST_SIG_NAME), pair):
			with tempfile.NamedTemporaryFile(dir=directory, prefix=name + ".", suffix=".tmp", delete=False) as temporary:
				temporary_paths.append((Path(temporary.name), directory / name))
				temporary.write(content)
		for temporary, destination in temporary_paths:
			temporary.replace(destination)
	finally:
		for temporary, _ in temporary_paths:
			temporary.unlink(missing_ok=True)


def load_stage_helper():
	script = Path(__file__).with_name("prepare_release_stage.py")
	spec = importlib.util.spec_from_file_location("tclient_release_stage", script)
	if spec is None or spec.loader is None:
		raise PackageError("could not load the release staging validator")
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return module


def verify_windows_authenticode(source: Path) -> dict:
	if os.name != "nt":
		raise PackageError("Windows release packaging requires Windows Authenticode verification")
	powershell = shutil.which("powershell.exe")
	if not powershell:
		raise PackageError("Windows Authenticode verification requires powershell.exe")
	script = Path(__file__).with_name("windows_sign_release.ps1")
	try:
		result = subprocess.run(
			[powershell, "-NoProfile", "-NonInteractive", "-File", str(script), "-Directory", str(source), "-Mode", "Verify"],
			capture_output=True, text=True, encoding="utf-8-sig", errors="replace", timeout=300,
		)
	except (OSError, subprocess.TimeoutExpired) as error:
		raise PackageError(f"could not verify Windows code signatures: {error}") from error
	if result.returncode != 0:
		raise PackageError(f"Windows code signature verification failed: {result.stderr.strip() or 'verification tool failed'}")
	try:
		report = json.loads(result.stdout)
	except ValueError as error:
		raise PackageError("Windows code signature verifier did not return a valid report") from error
	if not isinstance(report, dict) or report.get("verified") is not True or report.get("mode") != "Verify":
		raise PackageError("Windows code signature verifier did not confirm verification")
	return report


def verify_windows_stage(source: Path, inventory_path: Path) -> dict:
	helper = load_stage_helper()
	try:
		inventory = helper.validate_stage(source, inventory_path, allow_first_party_changes=True)
		verify_windows_authenticode(source)
		# Verification never licenses a change to a vendor file or a game resource.
		helper.validate_stage(source, inventory_path, allow_first_party_changes=True)
		return inventory
	except helper.StageError as error:
		raise PackageError(f"release stage provenance verification failed: {error}") from error


def archive_entries(archive: zipfile.ZipFile) -> dict[str, zipfile.ZipInfo]:
	entries = {}
	folded = set()
	total_size = 0
	for entry in archive.infolist():
		name = entry.filename
		parts = name.rstrip("/").split("/")
		if not name or "\\" in entry.orig_filename or ":" in name or any(not part or part in (".", "..") or part.rstrip(" .") != part for part in parts):
			raise PackageError(f"unsafe path in release archive: {name!r}")
		if stat.S_ISLNK(entry.external_attr >> 16):
			raise PackageError(f"symlink in release archive: {name}")
		if entry.is_dir():
			continue
		if name.casefold() in folded:
			raise PackageError(f"duplicate path in release archive: {name}")
		folded.add(name.casefold())
		total_size += entry.file_size
		if len(folded) > 100000 or entry.file_size > 1024 ** 3 or total_size > 8 * 1024 ** 3:
			raise PackageError("release archive exceeds extraction limits")
		entries[name] = entry
	return entries


def verify_zip_contents(path: Path, files: list[tuple[str, Path]]) -> None:
	expected = dict(files)
	try:
		with zipfile.ZipFile(path) as archive:
			entries = archive_entries(archive)
			if set(entries) != set(expected):
				raise PackageError(f"release archive inventory mismatch: {path}")
			for name, entry in entries.items():
				if entry.file_size != expected[name].stat().st_size:
					raise PackageError(f"release archive size mismatch: {name}")
				with archive.open(entry) as content:
					digest = hashlib.file_digest(content, "sha256").hexdigest()
				if digest != sha256_of(expected[name]):
					raise PackageError(f"release archive byte mismatch: {name}")
	except (zipfile.BadZipFile, RuntimeError) as error:
		raise PackageError(f"invalid release archive {path}: {error}") from error


def verify_packaged_windows(output: Path, full_path: Path, inventory_path: Path) -> None:
	helper = load_stage_helper()
	try:
		inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
		if not isinstance(inventory, dict) or not isinstance(inventory.get("files"), list):
			raise PackageError("invalid release stage inventory")
		with tempfile.TemporaryDirectory(prefix="tclient-authenticode-verify-") as temporary:
			source = Path(temporary)
			with zipfile.ZipFile(full_path) as archive:
				for name, entry in archive_entries(archive).items():
					destination = source.joinpath(*name.split("/"))
					destination.parent.mkdir(parents=True, exist_ok=True)
					with archive.open(entry) as content, destination.open("xb") as target:
						shutil.copyfileobj(content, target)
			# Validate before filtering: malformed inventory records must never disappear.
			helper.validate_files(source, inventory, allow_first_party_changes=True, require_stage_root=False)
			verify_windows_authenticode(source)
			update_files = collect_files(source, UPDATE_DATA_SUBDIRS)
			verify_zip_contents(output / ASSET_NAME, update_files)
			if (output / HELPER_NAME).read_bytes() != (source / HELPER_NAME).read_bytes():
				raise PackageError("standalone helper differs from the signed full package")
			client = output / "DDNet.exe"
			if not client.is_file() or client.read_bytes() != (source / "DDNet.exe").read_bytes():
				raise PackageError("standalone client differs from the signed full package")
	except helper.StageError as error:
		raise PackageError(f"release archive provenance verification failed: {error}") from error
	except (ValueError, zipfile.BadZipFile, RuntimeError) as error:
		raise PackageError(f"invalid Windows release package: {error}") from error


def parse_arguments(argv: list[str]) -> argparse.Namespace:
	parser = argparse.ArgumentParser(
		description="Create or verify the signed TClient Windows autoupdate assets.",
		formatter_class=argparse.RawTextHelpFormatter,
	)
	parser.add_argument("--verify", action="store_true", help="Read-only Ed25519 gate for existing assets; add --require-authenticode for the Windows release gate")
	parser.add_argument("--require-authenticode", action="store_true", help="With --verify: also verify Windows signatures, provenance and full/update/standalone byte equality")
	parser.add_argument("--stage-inventory", help="Required when packaging or verifying Authenticode: inventory from prepare_release_stage.py")
	parser.add_argument("--source", help="Fresh client directory prepared by prepare_release_stage.py and signed by windows_sign_release.ps1")
	parser.add_argument("--version", required=True, help="Expected client version, e.g. 1.7.8.54 (also required when verifying)")
	parser.add_argument("--output", required=True, help="Directory containing the four release assets")
	parser.add_argument("--raw-manifest-dir", help="Exact raw directory, e.g. repo/update: copy the verified signed pair when packaging; compare it read-only with --verify")
	parser.add_argument("--raw-manifest-url", help="With --verify only: HTTPS URL of the deployed manifest; its .sig is fetched too")
	parser.add_argument("--notes", default=None, help="Release notes stored in the manifest")
	parser.add_argument("--notes-file", default=None, help="Read the release notes from this file instead")
	parser.add_argument("--private-key", help="Required Ed25519 private key PEM matching the client's embedded public key")
	parser.add_argument("--product", default="TClient", help="Product name in the manifest (default: TClient)")
	parser.add_argument("--channel", default="stable", help="Update channel in the manifest (default: stable)")
	parser.add_argument("--platform", default="win64", help="Platform in the manifest (default: win64)")
	parser.add_argument(
		"--include-data",
		default="",
		help="Comma separated data/ subdirectories the update must also ship, e.g. shader.\n"
		"By default only data/tclient travels with an update, all other game\n"
		"resources are left untouched on the player's installation.",
	)
	parser.add_argument(
		"--full-zip",
		default=None,
		help="Also write a full installation package (every game resource) to this\n"
		"path, e.g. K:/out/TClient-v1.7.8.35-full-win64.zip. It is never consumed\n"
		"by the updater and needs no manifest.",
	)
	parser.add_argument("--quiet", action="store_true", help="Only print the manifest and the summary")
	args = parser.parse_args(argv)
	if args.verify:
		if args.source or args.private_key or args.notes is not None or args.notes_file or args.include_data:
			parser.error("--verify does not accept packaging options (--source, --private-key, --notes, --notes-file, --include-data)")
		if args.require_authenticode:
			if not args.stage_inventory or not args.full_zip:
				parser.error("--require-authenticode requires --stage-inventory and --full-zip")
		elif args.full_zip or args.stage_inventory:
			parser.error("--verify requires --require-authenticode when using --full-zip or --stage-inventory")
	else:
		if not args.source:
			parser.error("--source is required when packaging")
		if args.raw_manifest_url:
			parser.error("--raw-manifest-url requires --verify")
	return args


def main(argv: list[str]) -> int:
	args = parse_arguments(argv)
	output = Path(args.output).resolve()
	expected_metadata = {"product": args.product, "channel": args.channel, "version": args.version, "platform": args.platform}

	try:
		if args.verify:
			pair = verify_release_assets(output, expected_metadata)
			if args.require_authenticode:
				verify_packaged_windows(output, Path(args.full_zip).resolve(), Path(args.stage_inventory).resolve())
			# Old releases need not contain output/update; if present it must agree too.
			if (output / "update").exists():
				verify_matching_pair(read_manifest_pair(output / "update"), pair, expected_metadata, str(output / "update"))
			if args.raw_manifest_dir:
				raw_dir = Path(args.raw_manifest_dir).resolve()
				verify_matching_pair(read_manifest_pair(raw_dir), pair, expected_metadata, str(raw_dir))
			if args.raw_manifest_url:
				verify_matching_pair(read_remote_manifest_pair(args.raw_manifest_url), pair, expected_metadata, "deployed raw manifest")
			print(f"verified release {args.version}: {output}")
			if args.raw_manifest_dir:
				print(f"verified raw manifest directory: {Path(args.raw_manifest_dir).resolve()}")
			if args.raw_manifest_url:
				print(f"verified deployed raw manifest: {args.raw_manifest_url}")
			return 0

		source = Path(args.source).resolve()
		private_key = load_signing_key(args.private_key)
		if any(not value.strip() for value in expected_metadata.values()):
			raise PackageError("release product, channel, version and platform must not be empty")
		if args.full_zip and Path(args.full_zip).suffix.lower() != ".zip":
			raise PackageError(f"--full-zip must end in .zip: {args.full_zip}")
		extra_data = tuple(part.strip().lower() for part in args.include_data.split(",") if part.strip())
		update_data = tuple(dict.fromkeys(UPDATE_DATA_SUBDIRS + extra_data))
		files = collect_files(source, update_data)
		if not files:
			raise PackageError(f"no files to package found in {source}")
		missing = [name for name in REQUIRED_FILES if name not in {relative for relative, _ in files}]
		if missing:
			raise PackageError(f"required files missing from the package: {', '.join(missing)}")

		# Validate the bootstrap dependency before writing a zip or manifest.
		helper_path = source / HELPER_NAME
		if not helper_path.is_file() or helper_path.stat().st_size == 0:
			raise PackageError(f"nonempty bootstrap helper missing from the package: {helper_path}")
		if not args.stage_inventory:
			raise PackageError("Windows release packaging requires --stage-inventory from prepare_release_stage.py")
		inventory_path = Path(args.stage_inventory).resolve()
		inventory = verify_windows_stage(source, inventory_path)
		full_files = collect_files(source, None)
		if {name for name, _ in full_files} != {entry["path"] for entry in inventory["files"]}:
			raise PackageError("release stage contains files excluded from the full package; prepare a clean stage")
		for destination in [output, Path(args.full_zip).resolve() if args.full_zip else None]:
			if destination is not None and (destination == source or source in destination.parents):
				raise PackageError("release outputs must be outside the prepared stage")
		verified_digests = {name: sha256_of(path) for name, path in full_files}
		helper_digest = sha256_of(helper_path)
		helper_size = helper_path.stat().st_size

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
		manifest["helper"] = {
			"name": HELPER_NAME,
			"size": helper_size,
			"sha256": helper_digest,
		}
		manifest_bytes = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8")

		signature = private_key.sign(manifest_bytes)
		manifest_path.write_bytes(manifest_bytes)
		(output / MANIFEST_SIG_NAME).write_bytes(signature)
		# The running client downloads this independently using the signed hash.
		helper_output = output / HELPER_NAME
		if helper_path.resolve() != helper_output.resolve():
			shutil.copyfile(helper_path, helper_output)
		client_output = output / "DDNet.exe"
		shutil.copyfile(source / "DDNet.exe", client_output)
		if not args.quiet:
			print("manifest signed with ed25519")

		if not args.quiet:
			print(f"source: {source}")
			print(f"data dirs in the update: {', '.join(update_data) if update_data else '(none)'}")
			print(f"files in the update: {len(files)}")
			for relative, absolute in files:
				print(f"  {relative} ({human_size(absolute.stat().st_size)})")

		# 全量安装包：含全部上游游戏资源，只给首次安装/修复用，更新器从不读它
		if args.full_zip:
			full_path = Path(args.full_zip).resolve()
			full_files = collect_files(source, None)
			full_missing = [name for name in REQUIRED_FILES if name not in {relative for relative, _ in full_files}]
			if full_missing:
				raise PackageError(f"required files missing from the full package: {', '.join(full_missing)}")
			full_path.parent.mkdir(parents=True, exist_ok=True)
			write_zip(full_files, full_path)
			full_size = full_path.stat().st_size
			full_digest = sha256_of(full_path)
			if not args.quiet:
				print(f"full package: {len(full_files)} files, {human_size(full_size)}")
		else:
			full_path = None

		# The manifest and raw pair are not publishable until archives contain the
		# exact verified bytes, including the complete Authenticode certificate table.
		if any(sha256_of(path) != verified_digests[name] for name, path in full_files):
			raise PackageError("release stage changed while packaging")
		verify_zip_contents(zip_path, files)
		if full_path is not None:
			verify_zip_contents(full_path, full_files)
		if client_output.read_bytes() != (source / "DDNet.exe").read_bytes():
			raise PackageError("standalone client differs from the signed stage")
		pair = verify_release_assets(output, expected_metadata)
		write_manifest_pair(output / "update", pair)
		if args.raw_manifest_dir:
			raw_dir = Path(args.raw_manifest_dir).resolve()
			write_manifest_pair(raw_dir, pair)
			verify_matching_pair(read_manifest_pair(raw_dir), pair, expected_metadata, str(raw_dir))

		print()
		print(f"zip:      {zip_path}")
		print(f"manifest: {manifest_path}")
		if args.raw_manifest_dir:
			print(f"raw manifest directory: {Path(args.raw_manifest_dir).resolve()}")
		print(f"helper:   {helper_size} bytes, sha256 {helper_digest[:16]}...")
		print(f"files:    {len(files)}")
		print(f"raw size: {human_size(uncompressed)}")
		print(f"zip size: {human_size(size)} ({size} bytes)")
		print(f"sha256:   {digest}")
		if args.full_zip:
			print(f"full zip: {full_path} ({human_size(full_size)}, {full_size} bytes)")
			print(f"full sha256: {full_digest}")
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
