"""Dedicated consumer UI. Shared installer engine; no developer workspace."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import queue
import shutil
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
import webbrowser
from texture_swapper import core
from texturequest_release import load_manifest, prepare_release, download, Paused
from texturequest_updates import prepare_update
from texturequest_direct import DirectSwapper, prepare_direct
from texturequest_latest import fetch_latest
from texturequest_version import APP_VERSION
from texturequest_version_check import check_updates as check_versions, texture_version, pack_identity, saved_manifest, installer_page

try:
    from texturequest_build_pin import APP_VERSION
except ImportError:
    if getattr(sys, "frozen", False):
        raise ValueError("Installer is missing its application version pin")
VERSION = APP_VERSION
SIDELOAD_WARNING = "We don't know what's more legal, replacing the game files or hooking the process. If Daybreak hates you, they might ban you. Be warned."
UPDATES = "https://github.com/Maergoth/TextureQuest/releases/latest"
BG, WHITE, INK, MUTED = "#f3f1ec", "#ffffff", "#23342f", "#67746d"
GREEN, DARK, GOLD, LINE = "#256851", "#172c26", "#dfc991", "#dedfd8"


def resource(name):
    path = Path(__file__).resolve().parent / name
    return path if path.is_file() else path.parent.parent / name


def state_directory():
    return Path(sys.executable).parent / "data" if getattr(sys, "frozen", False) else Path(__file__).resolve().parents[1] / "build/data"


def human_bytes(value):
    return f"{value / 1_000_000_000:.1f} GB" if value >= 1_000_000_000 else f"{value / 1_000_000:.1f} MB"


def installation_current(status, manifest):
    actual = {r.get("name", "").casefold(): r for r in status.get("archives", [])}
    for expected in manifest["archives"]:
        row = actual.get(expected["name"].casefold(), {})
        if (row.get("integrity") != "intact" or row.get("actual_sha256") != expected["sha256"]
                or row.get("actual_bytes") != expected["bytes"]):
            return False
    return bool(manifest["archives"])


def installation_summary(status, manifest):
    if installation_current(status, manifest):
        return "TextureQuest is installed and verified."
    if status.get("managed_count"):
        return "Installed files need attention. Reinstall TextureQuest after patching."
    return "TextureQuest has not been installed by this app yet."


def can_install(busy, prepared, preflight):
    return bool(not busy and prepared and preflight and not preflight.get("missing"))


def update_messages(result, app_version):
    app, pack = result.get("installer", {}), result.get("pack", {})
    if app.get("status") == "verified":
        app_text = (f"App update available: {app['version']} (running {app_version})." if app["update_available"] else
                    f"Running app {app_version}; GitHub has {app['version']}." if app["local_newer"] else
                    f"App {app_version} is current.")
    else:
        app_text = "App update check unavailable." if app.get("status") == "unavailable" else "App updates have not been checked."
    if pack.get("status") == "verified":
        pack_text = (f"New textures available: {pack['version']}. Download / Resume to prepare them." if pack["update_available"] else
                     f"Latest textures: {pack['version']}.")
    else:
        pack_text = "Texture update check unavailable." if pack.get("status") == "unavailable" else "Texture updates have not been checked."
    return app_text, pack_text


def download_space_needed(manifest, folder, prepared_cache=False):
    final, partial = folder / "Texture-Pack.zip", folder / "Texture-Pack.zip.download"
    saved = final.stat().st_size if final.is_file() else partial.stat().st_size if partial.is_file() else 0
    remaining = max(0, manifest["archive"]["bytes"] - saved)
    return remaining + (0 if prepared_cache else sum(row["bytes"] for row in manifest["archives"])) + 64 * 1024**2


def game_space_needed(preflight):
    growth = 0
    largest = 0
    for row in preflight["archives"]:
        current = core._plain(row["target"]).stat().st_size
        growth += max(0, row["bytes"] - current)
        largest = max(largest, row["bytes"])
    return growth + largest + 64 * 1024**2


class TextureQuestApp:
    def __init__(self, root, state_dir=None, initial_game=None, initial_pack=None, startup_check=True):
        self.root = root
        self.state_dir = core._plain(state_dir or state_directory())
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.core = DirectSwapper(self.state_dir)
        self.cache_guard = None  # Future sideload panel supplies its ownership guard.
        try:
            from texturequest_build_pin import MANIFEST_SHA256
        except ImportError:
            if getattr(sys, "frozen", False):
                raise ValueError("Installer is missing its bundled release pin")
            MANIFEST_SHA256 = None
        self.release = load_manifest(resource("release-manifest.json"), MANIFEST_SHA256)
        if getattr(sys, "frozen", False):
            from texturequest_build_pin import TEXTURE_VERSION
            if TEXTURE_VERSION != texture_version(self.release):
                raise ValueError("Installer texture version differs from its bundled manifest")
        self.app_version = APP_VERSION
        self.update_check = {}
        self.known_pack_versions = {pack_identity(self.release): texture_version(self.release)}
        try:
            self.release = saved_manifest(self.state_dir, self.release)
            self.known_pack_versions[pack_identity(self.release)] = texture_version(self.release)
        except (OSError, ValueError, TypeError, KeyError):
            pass  # Preserve unknown receipts. A fresh check is still required.
        try:
            saved = core._load(self.state_dir / "texturequest-versions.json")
            if saved.get("schema") != "texturequest-local-versions-v1" or not isinstance(saved.get("packs"), dict) or len(saved["packs"]) > 1000:
                raise ValueError("Invalid saved texture version history")
            from texturequest_latest import version_number
            import re
            for identity, version in saved["packs"].items():
                if not re.fullmatch(r"[0-9a-f]{64}", identity):
                    raise ValueError("Invalid saved texture identity")
                version_number(version)
                if identity in self.known_pack_versions and self.known_pack_versions[identity] != version:
                    raise ValueError("Saved texture version disagrees with known bytes")
            self.known_pack_versions.update(saved["packs"])
        except (FileNotFoundError, ValueError, OSError, KeyError, TypeError):
            pass  # History is display-only; never use it to authorize installation.
        self.release_checked = False
        self.events = queue.Queue()
        self.busy = None
        self.prepared = self.preflight = None
        self.installed = {}
        self.pause = threading.Event()
        self.closing = self.close_after_pause = False
        self.last_progress = 0
        self.controls = []
        self.game = tk.StringVar(value=str(initial_game or ""))
        self.game.trace_add("write", lambda *_: self.game_changed())
        root.title("TextureQuest")
        root.geometry(f"840x{min(870, max(640, root.winfo_screenheight() - 80))}")
        root.minsize(760, 640)
        root.configure(bg=BG)
        root.option_add("*Font", ("Segoe UI", 10))
        self.build()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(80, self.poll)
        self.initial_pack = initial_pack
        if startup_check:
            root.after(200, self.check_updates)

    def label(self, parent, text, size=10, color=INK, bold=False, **kwargs):
        return tk.Label(parent, text=text, bg=parent.cget("bg"), fg=color, anchor="w",
                        font=("Segoe UI Semibold" if bold else "Segoe UI", size), **kwargs)

    def button(self, parent, text, command, primary=False):
        button = tk.Button(parent, text=text, command=command, bg=GREEN if primary else "#e8ede6",
                           fg=WHITE if primary else INK, activebackground="#1b513e" if primary else "#dce4d9",
                           activeforeground=WHITE if primary else INK, disabledforeground="#929e96",
                           font=("Segoe UI Semibold", 10), relief="flat", borderwidth=0, cursor="hand2", padx=20, pady=11)
        self.controls.append(button)
        return button

    def card(self, parent, row, number, title):
        outer = tk.Frame(parent, bg=WHITE, highlightbackground=LINE, highlightthickness=1)
        outer.grid(row=row, column=0, sticky="ew", pady=(0, 12))
        outer.columnconfigure(1, weight=1)
        self.label(outer, number, size=20, bold=True, color=GREEN).grid(row=0, column=0, sticky="n", padx=(18, 14), pady=16)
        body = tk.Frame(outer, bg=WHITE, pady=11)
        body.grid(row=0, column=1, sticky="ew", padx=(0, 20))
        body.columnconfigure(0, weight=1)
        self.label(body, title, size=13, bold=True).grid(row=0, column=0, sticky="w")
        return body

    def build(self):
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure("Quest.Horizontal.TProgressbar", troughcolor="#e1e6dd", background=GREEN, borderwidth=0)
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)
        header = tk.Frame(self.root, bg=DARK, padx=28, pady=16)
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(0, weight=1)
        tk.Label(header, text="TextureQuest", font=("Georgia", 28), fg=GOLD, bg=DARK, anchor="w").grid(row=0, column=0, sticky="w")
        self.label(header, "More detail. The EverQuest II you remember.", color="#c7d3c9").grid(row=1, column=0, sticky="w", pady=(4, 0))
        self.version_label = self.label(header, "App " + self.app_version, color=GOLD)
        self.version_label.grid(row=0, column=1, sticky="ne", padx=(18, 0))
        self.app_update_label = self.label(header, "App updates have not been checked.", color="#c7d3c9", size=9, wraplength=610)
        self.app_update_label.grid(row=2, column=0, sticky="w", pady=(9, 0))
        self.installer_update_button = self.button(header, "Get app update", self.open_app_update)
        self.installer_update_button.grid(row=2, column=1, sticky="e", padx=(10, 0))
        self.installer_update_button.grid_remove()
        viewport = tk.Frame(self.root, bg=BG)
        viewport.grid(row=1, column=0, sticky="nsew")
        viewport.columnconfigure(0, weight=1)
        viewport.rowconfigure(0, weight=1)
        self.body_canvas = tk.Canvas(viewport, bg=BG, highlightthickness=0, height=500)
        self.body_canvas.grid(row=0, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(viewport, orient="vertical", command=self.body_canvas.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.body_canvas.configure(yscrollcommand=scrollbar.set)
        body = tk.Frame(self.body_canvas, bg=BG, padx=24, pady=16)
        body_window = self.body_canvas.create_window(0, 0, window=body, anchor="nw")
        body.bind("<Configure>", lambda _: self.body_canvas.configure(scrollregion=self.body_canvas.bbox("all")))
        self.body_canvas.bind("<Configure>", lambda event: self.body_canvas.itemconfigure(body_window, width=event.width))
        self.root.bind("<MouseWheel>", lambda event: self.body_canvas.yview_scroll(-int(event.delta / 120), "units"))
        body.columnconfigure(0, weight=1)
        game_card = self.card(body, 0, "1", "Choose your EverQuest II folder")
        self.label(game_card, "Patch first, then close the game and launcher.", color=MUTED).grid(row=1, column=0, sticky="w", pady=(4, 9))
        row = tk.Frame(game_card, bg=WHITE)
        row.grid(row=2, column=0, sticky="ew")
        row.columnconfigure(0, weight=1)
        self.game_entry = tk.Entry(row, textvariable=self.game, bg="#f2f4f0", fg=INK, relief="flat", insertbackground=INK)
        self.game_entry.grid(row=0, column=0, sticky="ew", ipady=11, padx=(0, 10))
        self.game_entry.bind("<Return>", lambda _: self.verify_installation())
        self.button(row, "Browse…", self.choose_game).grid(row=0, column=1)
        pack_card = self.card(body, 1, "2", "Get the complete texture pack")
        updated = self.release.get("schema") == "texturequest-release-v2"
        direct = self.release.get("schema") == "texturequest-release-v3"
        self.pack_description = self.label(pack_card, "Saved downloads are reused. Checking for updates never installs files.", color=MUTED, size=9)
        self.pack_description.grid(row=1, column=0, sticky="w", pady=(4, 9))
        row = tk.Frame(pack_card, bg=WHITE)
        row.grid(row=2, column=0, sticky="ew")
        row.columnconfigure(1, weight=1)
        self.download_button = self.button(row, "Download pack", self.download_pack)
        self.download_button.grid(row=0, column=0, sticky="w")
        self.download_label = self.label(row, "Not downloaded", color=MUTED, wraplength=350, justify="left")
        self.download_label.grid(row=0, column=1, sticky="w", padx=(14, 0))
        space = ("One saved texture set. Downloaded VPKs are ready for installation; no ZIP or extracted duplicate."
                 if direct else "Updates reuse matching files; fresh or patched installs may also need the 27.8 GB base."
                 if updated else "Allow 56 GB for download/cache, plus 16 GB free on the game drive.")
        self.label(pack_card, space, size=9, color=MUTED, wraplength=620, justify="left").grid(row=3, column=0, sticky="w", pady=(9, 0))
        self.pack_update_label = self.label(pack_card, "Texture updates have not been checked.", size=9, color=GREEN, wraplength=620, justify="left")
        self.pack_update_label.grid(row=4, column=0, sticky="w", pady=(8, 0))
        self.pack_versions_label = self.label(pack_card, "", size=9, color=MUTED, wraplength=620, justify="left")
        self.pack_versions_label.grid(row=5, column=0, sticky="w", pady=(3, 0))
        install_card = self.card(body, 2, "3", "Install TextureQuest")
        self.install_label = self.label(install_card, "Choose a game folder and prepare the download first.", color=MUTED, wraplength=610, justify="left")
        self.install_label.grid(row=1, column=0, sticky="w", pady=(4, 10))
        self.install_button = self.button(install_card, "Install TextureQuest", self.install, primary=True)
        self.install_button.grid(row=2, column=0, sticky="w")
        self.sideload = tk.BooleanVar(value=False)
        self.sideload_requested = False
        self.sideload.trace_add("write", lambda *_: setattr(self, "sideload_requested", self.sideload.get()))
        self.sideload_checkbox = tk.Checkbutton(install_card, text="Sideload (unavailable for this build)",
            variable=self.sideload, state="disabled", bg=WHITE, fg=MUTED, anchor="w", font=("Segoe UI", 9))
        self.sideload_checkbox.grid(row=3, column=0, sticky="w", pady=(8, 0))
        self.label(install_card, "No tested EQ2 texture-injection backend is included. Installation replaces game archives.",
            size=9, color=MUTED, wraplength=620, justify="left").grid(row=4, column=0, sticky="w")
        self.label(install_card, SIDELOAD_WARNING, size=9, color="#865d36", wraplength=620,
            justify="left").grid(row=5, column=0, sticky="w", pady=(5, 0))
        row = tk.Frame(body, bg=BG)
        row.grid(row=3, column=0, sticky="ew", pady=(1, 0))
        row.columnconfigure(0, weight=1)
        self.progress = ttk.Progressbar(row, mode="determinate", maximum=100, style="Quest.Horizontal.TProgressbar")
        self.progress.grid(row=0, column=0, sticky="ew")
        self.pause_button = tk.Button(row, text="Pause", command=self.request_pause, font=("Segoe UI", 9), bg="#e8ede6", fg=INK, relief="flat", padx=10, pady=3)
        self.pause_button.grid(row=0, column=1, padx=(10, 0))
        self.pause_button.grid_remove()
        self.message = self.label(body, "Your game files change only when you click Install.", color=MUTED, wraplength=740, justify="left")
        self.message.grid(row=4, column=0, sticky="ew", pady=(9, 0))
        body.rowconfigure(5, weight=1)
        footer = tk.Frame(self.root, bg=BG, padx=24, pady=12)
        footer.grid(row=2, column=0, sticky="ew")
        footer.columnconfigure(0, weight=1)
        self.label(footer, "After a game patch, rerun this installer. Saved textures are reused.", size=9, color=MUTED).grid(row=0, column=0, sticky="w")
        self.menu_button = tk.Menubutton(footer, text="Maintenance & updates ▾", bg=BG, fg=GREEN, activebackground="#e8ede6", relief="flat", cursor="hand2", padx=8, pady=6)
        self.menu_button.grid(row=1, column=0, sticky="w", pady=(5, 0))
        self.menu = tk.Menu(self.menu_button, tearoff=False, bg=WHITE, fg=INK)
        self.menu_button.configure(menu=self.menu)
        self.menu.add_command(label="Verify installed textures", command=self.verify_installation)
        self.menu.add_command(label="Return to original textures", command=self.return_to_stock)
        self.menu.add_separator()
        self.menu.add_command(label="Clear downloaded files & cache…", command=self.clear_cache)
        self.menu.add_separator()
        self.menu.add_command(label="Check for updates", command=self.check_updates)
        self.menu.add_command(label="Instructions", command=lambda: webbrowser.open("https://github.com/Maergoth/TextureQuest#download-and-install"))
        self.refresh_controls()

    def set_message(self, text, error=False):
        self.message.configure(text=str(text), fg="#9a492c" if error else MUTED)

    def game_changed(self):
        self.preflight, self.installed = None, {}
        if hasattr(self, "install_button"):
            self.refresh_controls()

    def saved_zip_path(self):
        prepared = self.prepared
        if not prepared:
            pointer = self.state_dir / "texturequest-prepared.json"
            try:
                prepared = core._load(pointer) if pointer.exists() else {}
            except (ValueError, OSError, KeyError):
                prepared = {}
        for source in prepared.get("source_paths", []):
            try:
                path = core._plain(source)
                if path.is_file():
                    return path
            except (ValueError, OSError):
                pass
        return None

    def refresh_controls(self):
        for button in self.controls:
            button.configure(state="disabled" if self.busy else "normal")
        self.game_entry.configure(state="disabled" if self.busy else "normal")
        self.menu_button.configure(state="disabled" if self.busy else "normal")
        self.install_button.configure(state="normal" if self.release_checked and can_install(self.busy, self.prepared, self.preflight) else "disabled")
        self.version_label.configure(text="App " + self.app_version)
        app_text, pack_text = update_messages(self.update_check, self.app_version)
        self.app_update_label.configure(text=app_text)
        self.pack_update_label.configure(text=pack_text)
        app = self.update_check.get("installer", {})
        if app.get("status") == "verified" and app.get("update_available"):
            self.installer_update_button.grid()
        else:
            self.installer_update_button.grid_remove()
        prepared = self.prepared
        if not prepared:
            try:
                prepared = core._load(self.state_dir / "texturequest-prepared.json")
            except (ValueError, OSError, KeyError):
                prepared = {}
        try:
            version = self.known_pack_versions.get(pack_identity(prepared))
        except (ValueError, KeyError, TypeError):
            version = None
        cache_text = ("Prepared " if self.prepared else "Saved ") + "textures " + version if version else "No identified saved pack"
        if version and not self.prepared:
            cache_text += " (verify before installing)"
        rows = [row for row in self.installed.get("archives", []) if row.get("managed")]
        try:
            installed_version = self.known_pack_versions.get(pack_identity({"archives": rows}))
        except (ValueError, KeyError, TypeError):
            installed_version = None
        installed_text = "Installed version not checked"
        if installed_version:
            installed_text = "Installed textures " + installed_version
            installed_text += " (verified)" if all(row.get("integrity") == "intact" for row in rows) else " (files need attention)"
        elif self.installed and not self.installed.get("managed_count"):
            installed_text = "No pack installed by this app"
        self.pack_versions_label.configure(text="Selected textures " + texture_version(self.release) + " · " + cache_text + "\n" + installed_text)
        current = installation_current(self.installed, self.release)
        self.install_button.configure(text="Reinstall TextureQuest" if current else "Install TextureQuest")
        if self.prepared:
            self.download_label.configure(text="Saved texture files verified" if self.prepared.get("schema") == "texturequest-direct-prepared-v1" else "Downloaded and verified")
            self.download_button.configure(text="Verify saved pack")
        else:
            folder = self.state_dir / "downloads"
            if self.release.get("schema") == "texturequest-release-v3":
                caption, state = "Download / Resume", "Matching saved VPKs are reused; each file is downloaded once"
            elif self.release.get("schema") == "texturequest-release-v2":
                caption, state = "Download / Resume", "Saved downloads and matching files will be reused"
            elif self.saved_zip_path() or (folder / "Texture-Pack.zip").is_file():
                caption, state = "Prepare saved pack", "Saved download available"
            elif (folder / "Texture-Pack.zip.download").is_file():
                caption, state = "Resume download", "Download paused or interrupted"
            else:
                caption, state = "Download pack", "Not downloaded"
            self.download_label.configure(text=state)
            self.download_button.configure(text=caption)
        if current:
            text = "Installed and verified. Rerun this installer after a game patch."
        elif self.preflight and self.preflight.get("missing"):
            text = "Required archives are missing. Patch this game folder fully, then try again."
        elif self.prepared and self.game.get().strip() and self.preflight:
            text = "Ready. Close EverQuest II and its launcher, then click Install."
        elif not self.game.get().strip():
            text = "Choose the game folder containing EverQuest2.exe and paks."
        else:
            text = "Prepare the downloaded pack to enable installation."
        self.install_label.configure(text=text)
        self.menu.entryconfigure(0, state="normal" if self.game.get().strip() else "disabled")
        self.menu.entryconfigure(1, state="normal" if self.game.get().strip() else "disabled")

    def open_app_update(self):
        app = self.update_check.get("installer", {})
        if app.get("status") == "verified" and app.get("update_available"):
            # This URL was verified against the fixed repository and published tag.
            # Open the release page so users keep control of downloading/upgrading.
            webbrowser.open(installer_page(app))

    def remember_pack_version(self):
        self.known_pack_versions[pack_identity(self.release)] = texture_version(self.release)
        core._save(self.state_dir / "texturequest-versions.json",
                   {"schema": "texturequest-local-versions-v1", "packs": self.known_pack_versions})

    def begin(self, kind, message, action):
        if self.busy:
            return
        self.busy = kind
        self.set_message(message)
        self.refresh_controls()
        if kind in ("download", "update"):
            self.pause.clear()
            self.progress.configure(mode="determinate", value=0, maximum=100)
            self.pause_button.configure(text="Pause", state="normal")
            self.pause_button.grid()
        else:
            self.progress.configure(mode="indeterminate")
            self.progress.start(12)
        def work():
            try:
                self.events.put(("done", kind, action()))
            except Exception as error:
                self.events.put(("error", kind, error))
        threading.Thread(target=work, daemon=True).start()

    def report(self, text):
        self.events.put(("progress", str(text)))

    def inspect(self, game, prepared=None):
        if not game:
            return {}, None
        core.validate_game(game)
        return self.core.status(game), self.core.preflight(prepared, game) if prepared else None

    def choose_game(self):
        folder = filedialog.askdirectory(title="Choose the folder containing EverQuest2.exe and paks", initialdir=self.game.get() or None)
        if folder:
            self.game.set(folder)
            self.verify_installation()

    def verify_installation(self):
        game, prepared = self.game.get().strip(), self.prepared
        if not game:
            self.set_message("Choose your EverQuest II folder first.")
            return
        self.begin("verify", "Checking your game folder and installed textures…", lambda: self.inspect(game, prepared))

    def prepare_zip(self, path):
        self._download_latest(base_zip=path)

    def download_pack(self):
        if self.release.get("schema") == "texturequest-release-v2":
            self.download_update()
            return
        self._download_latest()

    def cache_in_use(self):
        # Background cache workers never query Tk state. An integrated panel
        # provides a thread-safe ownership snapshot through cache_guard.
        return bool(self.sideload_requested or (self.cache_guard is not None and self.cache_guard()))

    def download_update(self, base_zip=None):
        self._download_latest(base_zip)

    def check_updates(self):
        if self.busy:
            return
        self.release_checked = False
        self.update_check = {}
        self.begin("check", "Checking texture and app versions on GitHub…",
                   lambda: check_versions(self.release, self.app_version, self.state_dir))

    def _download_latest(self, base_zip=None):
        if self.busy:
            return
        game = self.game.get().strip()
        minimum = self.release["version"]
        self.release_checked = False
        def notify(message, done, total):
            now = time.monotonic()
            if now - self.last_progress > .12 or done == total or total == 1:
                self.events.put(("download-progress", message, done, total))
                self.last_progress = now
        def work():
            latest = fetch_latest(self.state_dir, minimum_version=minimum, pause=self.pause.is_set, current_manifest=self.release)
            if latest.get("schema") == "texturequest-release-v3":
                prepared = prepare_direct(self.core, latest, notify, self.pause.is_set,
                                          cache_in_use=self.cache_in_use)
            elif latest.get("schema") == "texturequest-release-v2":
                prepared = prepare_update(self.core, latest, game or None, base_zip,
                                          notify, self.pause.is_set)
            else:
                path = base_zip or self.saved_zip_path()
                if not path:
                    path = download(latest, self.state_dir / "downloads", notify, self.pause.is_set)
                prepared = prepare_release(self.core, path, latest, self.report)
            status, preflight = self.inspect(game, prepared)
            return latest, prepared, status, preflight
        self.begin("update", "Checking the latest release, then reusing saved files and downloading what is needed…", work)

    def request_pause(self):
        if self.busy in ("download", "update"):
            self.pause.set()
            self.pause_button.configure(text="Pausing…", state="disabled")
            self.set_message("Pausing safely. Downloaded data will be kept.")

    def install(self):
        if self.sideload.get():
            self.set_message("Sideload is unavailable: no tested texture-injection backend is included.", error=True)
            return
        if not self.release_checked or not can_install(self.busy, self.prepared, self.preflight):
            return
        prepared, game = self.prepared, self.game.get().strip()
        def work():
            ready = self.core.preflight(prepared, game)
            needed = game_space_needed(ready)
            if shutil.disk_usage(ready["paks_dir"]).free < needed:
                raise OSError(f"Free at least {human_bytes(needed)} on the game drive for the larger textures and a staged archive.")
            return self.core.install(prepared, game, progress=self.report)
        self.begin("install", "Installing TextureQuest. Keep the game and launcher closed until verification finishes…",
                   work)

    def return_to_stock(self):
        game = self.game.get().strip()
        def work():
            self.core.uninstall(game, progress=self.report)
            return self.core.status(game)
        self.begin("stock", "Preparing to return to original textures…", work)

    def clear_cache(self):
        if self.busy or self.cache_in_use():
            return
        if not messagebox.askyesno("Clear saved textures", "Remove this app's saved texture files?\n\nInstalled game textures and installation records stay intact. Reinstalling will need a new download.", parent=self.root):
            return
        def work():
            with core._lock(self.state_dir / "operation.lock"), core._lock(self.state_dir / "sideload/helper-owner.lock"):
                if self.cache_in_use():
                    raise RuntimeError("Stop sideload before clearing saved textures")
                for name in ("downloads", "prepared", "explorer"):
                    target = core._plain(self.state_dir / name, self.state_dir)
                    if target.exists():
                        for entry in target.rglob("*"):
                            core._plain(entry, target)
                        shutil.rmtree(target)
                core._plain(self.state_dir / "texturequest-prepared.json", self.state_dir).unlink(missing_ok=True)
                core._plain(self.state_dir / "direct-prepared.json", self.state_dir).unlink(missing_ok=True)
        self.begin("clear", "Clearing downloaded files. Game textures and installation records are kept…", work)

    def poll(self):
        try:
            while True:
                event = self.events.get_nowait()
                if event[0] == "progress":
                    self.set_message(event[1])
                    continue
                if event[0] == "download-progress":
                    _, message, done, total = event
                    self.progress.configure(value=100 * done / max(total, 1))
                    self.download_label.configure(text=f"{100 * done / total:.1f}% · {human_bytes(done)} of {human_bytes(total)}")
                    self.set_message(message)
                    continue
                event_type, kind, value = event
                self.busy = None
                self.progress.stop()
                self.progress.configure(mode="determinate", value=0, maximum=100)
                self.pause_button.grid_remove()
                if event_type == "error":
                    self.set_message(str(value), error=not isinstance(value, Paused))
                    if kind in ("install", "stock") and self.game.get().strip():
                        self.recover_message = str(value)
                        game, prepared = self.game.get().strip(), self.prepared
                        self.begin("recover", str(value), lambda: self.inspect(game, prepared))
                elif kind == "download":
                    if not self.close_after_pause:
                        self.prepare_zip(value)
                elif kind in ("prepare", "update"):
                    if kind == "update":
                        self.release, self.prepared, self.installed, self.preflight = value
                        self.release_checked = True
                    else:
                        self.prepared, self.installed, self.preflight = value
                    self.remember_pack_version()
                    if self.update_check.get("pack", {}).get("status") == "verified":
                        self.update_check["pack"]["update_available"] = False
                    self.set_message("Pack verified. Click Install when the game and launcher are closed.")
                elif kind == "check":
                    self.update_check = value
                    pack = value["pack"]
                    if pack.get("status") == "verified":
                        if pack_identity(pack["manifest"]) != pack_identity(self.release):
                            self.prepared = self.preflight = None
                        self.release = pack["manifest"]
                        self.release_checked = True
                        self.remember_pack_version()
                    failures = [channel["message"] for channel in (value["pack"], value["installer"]) if channel.get("status") == "unavailable"]
                    self.set_message(" ".join(dict.fromkeys(failures)) if failures else
                                     "Version checks complete. App updates open on GitHub; texture downloads are prepared here.", error=bool(failures))
                    if self.release_checked and self.game.get().strip():
                        if self.initial_pack:
                            initial, self.initial_pack = self.initial_pack, None
                            self.prepare_zip(initial)
                        else:
                            self.verify_installation()
                    elif self.release_checked and self.initial_pack:
                        initial, self.initial_pack = self.initial_pack, None
                        self.prepare_zip(initial)
                elif kind in ("verify", "recover"):
                    self.installed, self.preflight = value
                    self.set_message(self.recover_message if kind == "recover" else installation_summary(self.installed, self.release), error=kind == "recover")
                elif kind == "install":
                    self.installed = value
                    self.remember_pack_version()
                    self.set_message("TextureQuest is ready. After a game patch, rerun this installer to reapply the textures.")
                    self.progress.configure(value=100)
                elif kind == "stock":
                    self.installed = value
                    guidance = "Run the official EQ2 patcher to restore the original textures. Modified bytes remain until the patcher replaces them."
                    self.set_message(guidance)
                    messagebox.showinfo("Next: run the official patcher", guidance, parent=self.root)
                elif kind == "clear":
                    self.prepared = self.preflight = None
                    self.set_message("Downloaded files cleared. Game textures and installation records were kept.")
                self.refresh_controls()
                if self.close_after_pause and not self.busy:
                    self.closing = True
                    self.root.destroy()
                    return
        except queue.Empty:
            pass
        if not self.closing:
            self.root.after(100, self.poll)

    def close(self):
        if self.busy in ("download", "update"):
            self.close_after_pause = True
            self.request_pause()
        elif self.busy:
            self.set_message("Please let this operation finish before closing. Your progress is being recorded.")
        else:
            self.closing = True
            self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description="TextureQuest portable installer")
    parser.add_argument("--data-dir", type=Path)
    parser.add_argument("--game-dir", type=Path)
    parser.add_argument("--pack", type=Path)
    parser.add_argument("--smoke-test", action="store_true")
    parser.add_argument("--verify-runtime", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.verify_runtime:
        from texturequest_runtime_check import run
        try:
            run(args.verify_runtime)
        except Exception:
            # The receipt contains the failure. A windowed frozen executable
            # must exit nonzero instead of blocking a publisher on a crash dialog.
            raise SystemExit(1)
        return
    root = tk.Tk()
    app = TextureQuestApp(root, args.data_dir, args.game_dir, args.pack, startup_check=not args.smoke_test)
    if args.smoke_test:
        def smoke():
            root.update_idletasks()
            receipt = {"version": app.app_version, "texture_version": texture_version(app.release),
                       "app_update_status": app.app_update_label.cget("text"), "texture_update_status": app.pack_update_label.cget("text"),
                       "title": root.title(), "game": app.game.get(),
                       "install_enabled": app.install_button.cget("state") != "disabled", "download_button": app.download_button.cget("text"),
                       "width": root.winfo_width(), "height": root.winfo_height(), "release_sha256": core._digest(app.release),
                       "scrollable_body": True,
                       "sideload_enabled": app.sideload_checkbox.cget("state") != "disabled",
                       "game_files_touched": False, "interface": "texturequest-three-step-consumer", "explorer": False}
            (app.state_dir / "ui-smoke.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
            root.destroy()
        root.after(1000, smoke)
    root.mainloop()


if __name__ == "__main__":
    main()
