"""Verify a tested Windows bundle and create release assets without credentials."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import zipfile

REQUIRED = {"VERSION.json", "quant_price_live.exe", "START_QUANT_PRICE_LIVE.cmd",
            "WORKBENCH_GUIDE_RU.md", "WORKBENCH_GUIDE.html", "RELEASE_NOTES_RU.md",
            "Qt6Core.dll", "platforms/qwindows.dll", "platforms/qoffscreen.dll"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def path_name(name):
    name = name.replace("\\", "/")
    path = PurePosixPath(name)
    require(name and not path.is_absolute() and ".." not in path.parts and ":" not in name
            and str(path) == name and not any(ord(c) < 32 for c in name), "Unsafe bundle path")
    return name


def sha256(stream):
    digest = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(block)
    return digest.hexdigest()


def checksums(text):
    entries = {}
    for line in text.splitlines():
        digest, separator, name = line.partition("  ")
        require(separator and re.fullmatch(r"[a-fA-F0-9]{64}", digest), "Invalid checksum line")
        name = path_name(name)
        require(name not in entries, "Duplicate checksum entry")
        entries[name] = digest.lower()
    return entries


def verify(names, read, digest, expected_commit, expected_version):
    require(re.fullmatch(r"[a-f0-9]{40}", expected_commit), "Invalid expected commit")
    require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", expected_version), "Invalid version")
    require(REQUIRED | {"SHA256SUMS.txt"} <= set(names), "Incomplete Windows bundle")
    require(len(names) <= 2000 and len({n.casefold() for n in names}) == len(names), "Duplicate Windows paths")
    sums = checksums(read("SHA256SUMS.txt").decode("utf-8-sig"))
    require(set(sums) == set(names) - {"SHA256SUMS.txt"}, "Checksum file does not cover the complete bundle")
    for name, expected in sums.items():
        require(digest(name) == expected, "Bundle checksum mismatch: " + name)
    metadata = json.loads(read("VERSION.json").decode("utf-8-sig"))
    require(metadata["commit"] == expected_commit and metadata["version"] == expected_version
            and metadata["platform"] == "Windows x64", "Wrong build manifest")
    require(read("quant_price_live.exe")[:2] == b"MZ", "Missing Windows executable")
    return metadata


def verify_bundle(root, commit, version):
    root = Path(root)
    require(root.is_dir() and not root.is_symlink(), "Missing bundle directory")
    paths = list(root.rglob("*"))
    require(not any(p.is_symlink() for p in paths), "Bundle contains a symlink")
    names = sorted(path_name(p.relative_to(root).as_posix()) for p in paths if p.is_file())
    def digest(name):
        with (root / name).open("rb") as stream:
            return sha256(stream)
    return verify(names, lambda n: (root / n).read_bytes(), digest, commit, version)


def verify_archive(path, commit, version):
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        require(len(infos) <= 2000 and sum(i.file_size for i in infos) <= 512 * 1024 * 1024,
                "Release archive limits exceeded")
        require(not any(stat.S_ISLNK(i.external_attr >> 16) or i.flag_bits & 1 for i in infos),
                "Unsupported archive entry")
        names = [path_name(i.filename) for i in infos if not i.is_dir()]
        require(len(set(names)) == len(names), "Duplicate ZIP entries")
        def digest(name):
            with archive.open(name) as stream:
                return sha256(stream)
        return verify(names, archive.read, digest, commit, version)


def prepare_release(bundle, output, commit, version):
    verify_bundle(bundle, commit, version)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    asset = output / ("QuantPriceLive-" + version + "-Windows-x64.zip")
    with zipfile.ZipFile(asset, "w", zipfile.ZIP_DEFLATED, compresslevel=6, strict_timestamps=False) as archive:
        for path in sorted(Path(bundle).rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(bundle).as_posix())
    verify_archive(asset, commit, version)
    with asset.open("rb") as stream:
        digest = sha256(stream)
    checksum = asset.with_name(asset.name + ".sha256")
    checksum.write_text(digest + "  " + asset.name + "\n", encoding="utf-8")
    return {"version": version, "tag": "v" + version, "asset": str(asset),
            "checksum": str(checksum), "sha256": digest, "commit": commit}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--output", type=Path, default=Path("release-output"))
    parser.add_argument("--commit", required=True)
    parser.add_argument("--version")
    parser.add_argument("--verify-archive", type=Path)
    args = parser.parse_args()
    version = args.version
    if not version:
        match = re.search(r"project\(QuantPriceLive\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
                          Path("CMakeLists.txt").read_text(encoding="utf-8"))
        require(match, "Cannot read application version")
        version = match.group(1)
    if args.verify_archive:
        verify_archive(args.verify_archive, args.commit, version)
        print("Release archive verified: " + args.commit)
        return
    require(args.bundle, "Specify --bundle")
    result = prepare_release(args.bundle, args.output, args.commit, version)
    (args.output / "release-info.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
