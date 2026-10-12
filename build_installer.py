"""Build the public Windows portable app with PyInstaller (no game access).

Use Python from an isolated environment with src/texture_swapper/requirements-build.txt.
All outputs and caches stay under this checkout's ignored build/ and dist/ folders.
"""
from __future__ import annotations
import hashlib
import argparse
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent
SRC = ROOT / "src"
BUILD = ROOT / "build"
DEST = ROOT / "dist"
NAME = "TextureQuest"
VERSION = "1.1.1"


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    global BUILD, DEST
    parser = argparse.ArgumentParser(description="Build a manifest-pinned TextureQuest installer")
    parser.add_argument("--manifest", type=Path, default=ROOT / "release-manifest.json")
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--version", default=VERSION)
    args = parser.parse_args()
    output_root = ROOT
    if args.output_root:
        output = args.output_root.resolve()
        output_root = output
        BUILD, DEST = output / "build", output / "dist"
    if (DEST / NAME / "data").exists():
        raise RuntimeError("Preserve portable user data before rebuilding this output")
    for path in (DEST, BUILD, BUILD / "tmp", BUILD / "pyinstaller-cache"):
        path.mkdir(parents=True, exist_ok=True)
    manifest = args.manifest.resolve()
    sys.path.insert(0, str(SRC))
    from texturequest_release import load_manifest
    release = load_manifest(manifest)
    if release["version"] != args.version:
        raise ValueError("Requested installer version differs from its bundled manifest")
    stage = BUILD / "source"
    (stage / "texture_swapper").mkdir(parents=True, exist_ok=True)
    sources = sorted([*SRC.glob("*.py"), SRC / "texture_swapper/core.py", SRC / "texture_swapper/__init__.py",
                      SRC / "texture_swapper/requirements-build.txt"])
    source_hashes = {p.relative_to(ROOT).as_posix(): sha(p) for p in sources}
    manifest_hash = sha(manifest)
    for source in sources:
        shutil.copy2(source, stage / source.relative_to(SRC))
    for source in (ROOT / "build_installer.py", ROOT / "INSTALLER.txt"):
        source_hashes[source.relative_to(ROOT).as_posix()] = sha(source)
    bundled_manifest = stage / "release-manifest.json"
    shutil.copy2(manifest, bundled_manifest)
    (stage / "texturequest_build_pin.py").write_text(
        f'MANIFEST_SHA256 = "{manifest_hash}"\nVERSION = "{release["version"]}"\n', encoding="utf-8")
    env = dict(os.environ, TEMP=str(BUILD / "tmp"), TMP=str(BUILD / "tmp"),
               PYINSTALLER_CONFIG_DIR=str(BUILD / "pyinstaller-cache"),
               PYTHONNOUSERSITE="1", PYTHONDONTWRITEBYTECODE="1")
    command = [sys.executable, "-B", "-m", "PyInstaller", "--noconfirm", "--onedir", "--windowed", "--noupx",
               "--name", NAME, "--paths", str(stage), "--distpath", str(DEST), "--workpath", str(BUILD / "pyinstaller"),
               "--specpath", str(BUILD), "--add-data", str(bundled_manifest) + ":."]
    for name in ("torch", "transformers", "diffusers", "scipy", "matplotlib", "pandas", "nightly", "nightly_fast", "PIL", "numpy", "texture_swapper.app", "texture_swapper.explorer", "texture_swapper.self_check"):
        command += ["--exclude-module", name]
    command += [str(stage / "texturequest_app.py")]
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    if (sha(manifest) != manifest_hash or sha(bundled_manifest) != manifest_hash
            or any(sha(ROOT / name) != expected for name, expected in source_hashes.items())):
        raise RuntimeError("Source changed during build")
    folder = DEST / NAME
    shutil.copy2(ROOT / "INSTALLER.txt", folder / "START-HERE.txt")
    licenses = folder / "licenses"
    licenses.mkdir(exist_ok=True)
    versions = {}
    for name in ("psutil", "pyinstaller", "pyinstaller-hooks-contrib", "altgraph", "pywin32-ctypes"):
        dist = importlib.metadata.distribution(name)
        versions[name] = dist.version
        for item in dist.files or []:
            if ".dist-info" in str(item) and any(word in item.name.lower() for word in ("license", "copying", "notice")):
                source = Path(dist.locate_file(item))
                if source.is_file():
                    target = licenses / name / Path(*item.parts[1:])
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, target)
    for name in ("LICENSE.txt", "LICENSE"):
        source = Path(sys.base_prefix) / name
        if source.is_file():
            shutil.copy2(source, licenses / "PYTHON-LICENSE.txt")
            break
    files = sorted(p for p in folder.rglob("*") if p.is_file())
    receipt = {"application": NAME, "version": release["version"], "runtime": sys.version, "dependencies": versions,
               "release_manifest_sha256": manifest_hash,
               "source_sha256": source_hashes,
               "files": [{"path": p.relative_to(folder).as_posix(), "bytes": p.stat().st_size, "sha256": sha(p)} for p in files]}
    receipt_path = folder / "BUILD-MANIFEST.json"
    receipt_path.write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    archive = DEST / f"TextureQuest-Installer-{release['version']}-windows-x64.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zipout:
        for path in files + [receipt_path]:
            zipout.write(path, (Path(NAME) / path.relative_to(folder)).as_posix())
    result = {"zip": archive.relative_to(output_root).as_posix(), "bytes": archive.stat().st_size, "version": release["version"],
              "release_manifest_sha256": manifest_hash,
              "sha256": sha(archive), "executable": (folder / (NAME + ".exe")).relative_to(output_root).as_posix()}
    (BUILD / "installer-receipt.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
