"""Frozen-runtime acceptance with a disposable game fixture only."""
import json
from pathlib import Path
import struct
import tempfile
import zlib


def run(output):
    from texture_swapper import core
    from texturequest_release import load_manifest
    from texturequest_app import resource
    output = Path(output).absolute()
    output.parent.mkdir(parents=True, exist_ok=True)
    result = {"status": "error", "game_files_touched": False, "gui_opened": False}
    try:
        with tempfile.TemporaryDirectory(prefix="texturequest-runtime-", dir=output.parent) as tmp:
            folder = Path(tmp)
            game = folder / "fixture-game"
            (game / "paks").mkdir(parents=True)
            (game / "EverQuest2.exe").write_bytes(b"isolated runtime fixture")
            name, blob = b"textures/runtime.txt", b"TextureQuest fixture"
            packed = zlib.compress(struct.pack("<II", len(name) + 1, len(blob)) + name + b"\0" + blob)
            entry = struct.pack("<III", 0, len(packed), len(name)) + name
            index = zlib.compress(struct.pack("<II", len(entry) + 8, 1) + entry)
            body = struct.pack("<I", len(packed)) + packed
            vpk = body + struct.pack("<I", len(index) + 12) + index + struct.pack("<III", 0x33744, len(body), 2)
            archive = game / "paks/runtime.vpk"
            archive.write_bytes(vpk)
            core._set_readonly(archive, True)
            assert core._readonly(archive)
            core._set_readonly(archive, False)
            assert core.archive_read(archive, name.decode()) == blob
            swapper = core.Swapper(folder / "state")
            prepared = swapper.prepare([archive])
            assert not swapper.preflight(prepared, game)["missing"]
            try:
                from texturequest_build_pin import MANIFEST_SHA256
            except ImportError:
                import sys
                if getattr(sys, "frozen", False):
                    raise ValueError("Missing bundled release pin")
                MANIFEST_SHA256 = None
            manifest = load_manifest(resource("release-manifest.json"), MANIFEST_SHA256)
            result.update(status="passed", archive_read=True, readonly_roundtrip=True, prepared_archive=True,
                          release_sha256=core._digest(manifest), portable_runtime=True)
    except Exception as error:
        result["error"] = repr(error)
        raise
    finally:
        output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result
