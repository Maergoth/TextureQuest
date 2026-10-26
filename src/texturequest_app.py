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
from texturequest_launch import game_executable, launch_game as start_game
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
BG, WHITE, INK, MUTED = "#10171e", "#19242e", "#edf3f6", "#99aab7"
GREEN, DARK, GOLD, LINE = "#5bd6b0", "#10251e", "#a9ead4", "#2d3e4c"
BUTTON, HOVER, ERROR = "#293b4a", "#354e60", "#ffa58d"
INSTALL_SUCCESS_MESSAGE = "Textures Installed. Launch via the EQ2 EXE, not the launcher!"


def window_size(screen_width, screen_height, dpi):
    """Fit the compact dashboard inside the display, including desktop chrome."""
    scale = max(1.0, dpi / 96.0)
    return (min(round(960 * scale), max(1, screen_width - round(48 * scale))),
            min(round(640 * scale), max(1, screen_height - round(80 * scale))))


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
        return "TextureQuest is installed."
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
        self.bundled_release = self.release
        self.core.trusted_manifest = self.bundled_release
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
        self.restore_saved_ready()
        self.installed = {}
        self.pause = threading.Event()
        self.closing = self.close_after_pause = False
        self.last_progress = 0
        self.last_overall_progress = 0
        self.overall_active = False
        self.controls = []
        self.install_success_dialog = None
        self.game = tk.StringVar(value=str(initial_game or ""))
        self.game.trace_add("write", lambda *_: self.game_changed())
        root.title("TextureQuest")
        width, height = window_size(root.winfo_screenwidth(), root.winfo_screenheight(), root.winfo_fpixels("1i"))
        root.geometry(f"{width}x{height}")
        scale = max(1.0, root.winfo_fpixels("1i") / 96.0)
        root.minsize(min(round(820 * scale), width), min(round(520 * scale), height))
        root.resizable(True, True)
        root.configure(bg=BG)
        root.option_add("*Font", ("Segoe UI", 10))
        self.build()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(80, self.poll)
        self.initial_pack = initial_pack
        if startup_check:
            root.after(200, self.check_updates)

    def label(self, parent, text, size=10, color=INK, bold=False, **kwargs):
        label = tk.Label(parent, text=text, bg=parent.cget("bg"), fg=color, anchor="w",
                         font=("Segoe UI Semibold" if bold else "Segoe UI", size), **kwargs)
        if "wraplength" in kwargs:
            label.bind("<Configure>", lambda event: label.configure(wraplength=max(1, event.width - 2)))
        return label

    def button(self, parent, text, command, primary=False, track=True):
        button = tk.Button(parent, text=text, command=command, bg=GREEN if primary else BUTTON,
                           fg=DARK if primary else INK, activebackground="#8ae4c7" if primary else HOVER,
                           activeforeground=DARK if primary else INK, disabledforeground="#718491",
                           font=("Segoe UI Semibold", 10), relief="flat", borderwidth=0, cursor="hand2",
                           highlightthickness=1, highlightbackground=LINE, highlightcolor=GREEN, padx=14, pady=8)
        button.quest_primary = primary
        if track:
            self.controls.append(button)
        return button

    def button_state(self, button, enabled):
        button.configure(state="normal" if enabled else "disabled",
                         bg=GREEN if enabled and button.quest_primary else BUTTON,
                         fg=DARK if enabled and button.quest_primary else INK)

    def card(self, parent, row, number, title):
        outer = tk.Frame(parent, bg=WHITE, highlightbackground=LINE, highlightthickness=1)
        outer.grid(row=row, column=0, sticky="nsew")
        outer.columnconfigure(0, weight=1)
        body = tk.Frame(outer, bg=WHITE, padx=16, pady=12)
        body.grid(row=0, column=0, sticky="ew")
        body.columnconfigure(0, weight=1)
        heading = tk.Frame(body, bg=WHITE)
        heading.grid(row=0, column=0, sticky="ew")
        self.label(heading, number.zfill(2), size=9, bold=True, color=GREEN).pack(side="left", padx=(0, 9))
        self.label(heading, title, size=11, bold=True).pack(side="left")
        return body

    def build(self):
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure("Quest.Horizontal.TProgressbar", troughcolor=LINE, background=GREEN,
                        lightcolor=GREEN, darkcolor=GREEN, bordercolor=LINE,
                        troughrelief="flat", borderwidth=0, thickness=7)
        style.configure("Quest.Vertical.TScrollbar", background=BUTTON, troughcolor=BG,
                        arrowcolor=MUTED, borderwidth=0, arrowsize=10)
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)

        header = tk.Frame(self.root, bg=BG, padx=22, pady=14)
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(0, weight=1)
        brand = tk.Frame(header, bg=BG)
        brand.grid(row=0, column=0, sticky="w")
        self.label(brand, "TextureQuest", size=23, bold=True).pack(side="left")
        self.label(brand, "EVERQUEST II", size=9, color=GREEN).pack(side="left", padx=(15, 0), pady=(8, 0))
        tools = tk.Frame(header, bg=BG)
        tools.grid(row=0, column=1, sticky="e")
        self.version_label = self.label(tools, "App " + self.app_version, size=9, color=MUTED)
        self.version_label.pack(side="left", padx=(0, 15))
        self.menu_button = tk.Menubutton(tools, text="Settings  ▾", bg=BUTTON, fg=INK,
            activebackground=HOVER, activeforeground=INK, relief="flat", cursor="hand2",
            font=("Segoe UI", 9), padx=12, pady=7, highlightthickness=0)
        self.menu_button.pack(side="left")
        self.menu = tk.Menu(self.menu_button, tearoff=False, bg=WHITE, fg=INK,
                            activebackground=GREEN, activeforeground=DARK, font=("Segoe UI", 10))
        self.menu_button.configure(menu=self.menu)
        self.menu.add_command(label="Verify installed textures", command=self.verify_installation)
        self.menu.add_command(label="Return to original textures", command=self.return_to_stock)
        self.menu.add_separator()
        self.menu.add_command(label="Clear downloaded files & cache…", command=self.clear_cache)
        self.menu.add_separator()
        self.menu.add_command(label="Check for updates", command=self.check_updates)
        self.menu.add_command(label="Instructions", command=lambda: webbrowser.open("https://github.com/Maergoth/TextureQuest#download-and-install"))
        self.app_update_label = self.label(header, "App updates have not been checked.", color=MUTED, size=9, wraplength=700)
        self.app_update_label.grid(row=1, column=0, sticky="ew", pady=(3, 0))
        self.installer_update_button = self.button(header, "Get app update", self.open_app_update)
        self.installer_update_button.grid(row=1, column=1, sticky="e", padx=(12, 0))
        self.installer_update_button.grid_remove()

        body = tk.Frame(self.root, bg=BG, padx=22)
        body.grid(row=1, column=0, sticky="nsew")
        body.columnconfigure(0, weight=1)
        body.rowconfigure(1, weight=1)
        game_card = self.card(body, 0, "1", "Game folder")
        row = tk.Frame(game_card, bg=WHITE)
        row.grid(row=1, column=0, sticky="ew", pady=(9, 0))
        row.columnconfigure(0, weight=1)
        self.game_entry = tk.Entry(row, textvariable=self.game, bg=BG, fg=INK, relief="flat",
            insertbackground=GREEN, font=("Segoe UI", 10), highlightthickness=1,
            highlightbackground=LINE, highlightcolor=GREEN, selectbackground=GREEN, selectforeground=DARK)
        self.game_entry.grid(row=0, column=0, sticky="ew", ipady=8, padx=(0, 10))
        self.game_entry.bind("<Return>", lambda _: self.verify_installation())
        self.browse_button = self.button(row, "Browse…", self.choose_game)
        self.browse_button.grid(row=0, column=1)

        middle = tk.Frame(body, bg=BG)
        middle.grid(row=1, column=0, sticky="nsew", pady=(12, 0))
        middle.columnconfigure(0, weight=3, uniform="panels")
        middle.columnconfigure(1, weight=2, uniform="panels")
        middle.rowconfigure(0, weight=1)
        download_area = tk.Frame(middle, bg=BG)
        download_area.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        download_area.columnconfigure(0, weight=1)
        download_area.rowconfigure(0, weight=1)
        play_area = tk.Frame(middle, bg=BG)
        play_area.grid(row=0, column=1, sticky="nsew", padx=(6, 0))
        play_area.columnconfigure(0, weight=1)
        play_area.rowconfigure(0, weight=1)
        pack_card = self.card(download_area, 0, "2", "Texture pack")
        self.pack_description = self.label(pack_card, "", color=MUTED, size=9, wraplength=500, justify="left")
        self.pack_description.grid(row=1, column=0, sticky="ew", pady=(6, 10))
        self.download_button = self.button(pack_card, "Download pack", self.download_pack)
        self.download_button.grid(row=2, column=0, sticky="ew")
        self.download_label = self.label(pack_card, "Not downloaded", color=MUTED, size=9, wraplength=500, justify="left")
        self.download_label.grid(row=3, column=0, sticky="ew", pady=(8, 0))
        self.pack_update_label = self.label(pack_card, "Texture updates have not been checked.", size=9, color=GREEN, wraplength=500, justify="left")
        self.pack_update_label.grid(row=4, column=0, sticky="ew", pady=(8, 0))
        self.pack_versions_label = self.label(pack_card, "", size=9, color=MUTED, wraplength=500, justify="left")
        self.pack_versions_label.grid(row=5, column=0, sticky="ew", pady=(4, 0))

        install_card = self.card(play_area, 0, "3", "Install & play")
        self.install_label = self.label(install_card, "Choose a game folder and prepare the download first.",
                                       size=9, color=MUTED, wraplength=320, justify="left")
        self.install_label.grid(row=1, column=0, sticky="ew", pady=(6, 10))
        self.install_button = self.button(install_card, "Install TextureQuest", self.install, primary=True)
        self.install_button.grid(row=2, column=0, sticky="ew")
        tk.Frame(install_card, bg=LINE, height=1).grid(row=3, column=0, sticky="ew", pady=13)
        self.launch_button = self.button(install_card, "Launch Game", self.launch_game)
        self.launch_button.grid(row=4, column=0, sticky="ew")
        self.label(install_card, "Starts EverQuest2.exe directly.", size=9, color=MUTED,
                   wraplength=320, justify="left").grid(row=5, column=0, sticky="ew", pady=(6, 0))
        # The unavailable injection path stays guarded without occupying the UI.
        self.sideload = tk.BooleanVar(value=False)
        self.sideload_requested = False
        self.sideload.trace_add("write", lambda *_: setattr(self, "sideload_requested", self.sideload.get()))

        self.progress_panel = tk.Frame(self.root, bg=BG, padx=22, pady=14)
        self.progress_panel.grid(row=2, column=0, sticky="ew")
        self.progress_panel.columnconfigure(0, weight=1)
        self.progress = ttk.Progressbar(self.progress_panel, mode="determinate", maximum=100,
                                       style="Quest.Horizontal.TProgressbar")
        self.progress.grid(row=0, column=0, sticky="ew")
        self.pause_button = self.button(self.progress_panel, "Pause", self.request_pause, track=False)
        self.pause_button.configure(font=("Segoe UI", 9), padx=9, pady=3)
        self.pause_button.grid(row=0, column=1, padx=(10, 0))
        self.pause_button.grid_remove()
        self.overall_label = self.label(self.progress_panel, "", size=9, color=GREEN, wraplength=1000, justify="left")
        self.overall_label.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(4, 0))
        status = tk.Frame(self.progress_panel, bg=BG)
        status.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(4, 0))
        status.columnconfigure(0, weight=1)
        self.message = tk.Text(status, width=1, height=2, wrap="word", state="disabled", bg=BG, fg=MUTED,
                               font=("Segoe UI", 9), relief="flat", borderwidth=0, highlightthickness=0,
                               selectbackground=BUTTON, selectforeground=INK, padx=0, pady=0)
        self.message.grid(row=0, column=0, sticky="ew")
        self.status_scrollbar = ttk.Scrollbar(status, orient="vertical", command=self.message.yview,
                                             style="Quest.Vertical.TScrollbar")
        self.status_scrollbar.grid(row=0, column=1, sticky="ns")
        self.message.configure(yscrollcommand=self.status_view)
        self.message.bind("<MouseWheel>", self.scroll_status)
        self.set_message("Choose your game folder to get started.")
        self.refresh_controls()

    def set_message(self, text, error=False):
        text = str(text)
        self.message.configure(state="normal", fg=ERROR if error else MUTED)
        try:
            self.message.delete("1.0", "end")
            self.message.insert("1.0", text)
            self.message.yview_moveto(0)
        finally:
            self.message.configure(state="disabled")

    def scroll_status(self, event):
        self.message.yview_scroll(-int(event.delta / 120), "units")
        return "break"

    def status_view(self, first, last):
        self.status_scrollbar.set(first, last)
        if float(first) <= 0 and float(last) >= 1:
            self.status_scrollbar.grid_remove()
        else:
            self.status_scrollbar.grid()

    def launch_game(self):
        if self.busy:
            return False
        try:
            start_game(self.game.get().strip())
        except (OSError, ValueError, RuntimeError) as error:
            self.set_message(str(error), error=True)
            messagebox.showerror("Couldn't launch EverQuest II", str(error),
                                 parent=self.install_success_dialog or self.root)
            return False
        self.set_message("EverQuest II launched using EverQuest2.exe.")
        return True

    def show_install_success(self):
        if self.install_success_dialog is not None and self.install_success_dialog.winfo_exists():
            self.install_success_dialog.lift()
            return self.install_success_dialog
        dialog = self.install_success_dialog = tk.Toplevel(self.root)
        dialog.withdraw()
        dialog.title("Textures installed")
        dialog.configure(bg=WHITE)
        dialog.transient(self.root)
        dialog.resizable(False, False)
        dialog.attributes("-alpha", self.root.attributes("-alpha"))
        body = tk.Frame(dialog, bg=WHITE, padx=24, pady=22)
        body.pack(fill="both", expand=True)
        self.label(body, "Ready for Norrath", size=18, bold=True, color=GREEN).pack(anchor="w")
        self.label(body, INSTALL_SUCCESS_MESSAGE, size=10, wraplength=420,
                   justify="left").pack(fill="x", pady=(12, 20))
        buttons = tk.Frame(body, bg=WHITE)
        buttons.pack(fill="x")

        def dismiss():
            self.install_success_dialog = None
            dialog.destroy()

        def launch():
            if self.launch_game():
                dismiss()

        self.success_close_button = self.button(buttons, "Close", dismiss, track=False)
        self.success_close_button.pack(side="right")
        self.success_launch_button = self.button(buttons, "Launch Game", launch, primary=True, track=False)
        self.success_launch_button.pack(side="right", padx=(0, 10))
        try:
            game_executable(self.game.get().strip())
            enabled = not self.busy
        except (OSError, ValueError):
            enabled = False
        self.button_state(self.success_launch_button, enabled)
        dialog.protocol("WM_DELETE_WINDOW", dismiss)
        dialog.bind("<Escape>", lambda _: dismiss())
        dialog.bind("<Return>", lambda _: launch() if enabled else dismiss())
        dialog.update_idletasks()
        x = self.root.winfo_rootx() + max(0, (self.root.winfo_width() - dialog.winfo_reqwidth()) // 2)
        y = self.root.winfo_rooty() + max(0, (self.root.winfo_height() - dialog.winfo_reqheight()) // 2)
        dialog.geometry(f"+{x}+{y}")
        if self.root.winfo_viewable():
            dialog.deiconify()
            dialog.grab_set()
            (self.success_launch_button if enabled else self.success_close_button).focus_set()
        return dialog

    def game_changed(self):
        self.preflight, self.installed = None, {}
        if hasattr(self, "install_button"):
            self.refresh_controls()

    def restore_saved_ready(self):
        """Bundled manifest plus an owned matching ready receipt permits offline use."""
        if pack_identity(self.release) != pack_identity(self.bundled_release):
            return False
        saved = self.core.saved_ready(self.bundled_release)
        if saved is None:
            return False
        self.prepared = saved
        self.release_checked = True
        return True

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
            self.button_state(button, not self.busy)
        self.game_entry.configure(state="disabled" if self.busy else "normal")
        self.menu_button.configure(state="disabled" if self.busy else "normal")
        self.button_state(self.install_button, self.release_checked and can_install(self.busy, self.prepared, self.preflight))
        try:
            game_executable(self.game.get().strip())
            launch_enabled = not self.busy
        except (OSError, ValueError):
            launch_enabled = False
        self.button_state(self.launch_button, launch_enabled)
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
            installed_text += " (installed)" if all(row.get("integrity") == "intact" for row in rows) else " (files need attention)"
        elif self.installed and not self.installed.get("managed_count"):
            installed_text = "No pack installed by this app"
        self.pack_versions_label.configure(text="Textures " + texture_version(self.release) + " · " + cache_text + "\n" + installed_text)
        self.pack_description.configure(text=f"{self.release.get('texture_count', 0):,} textures · {len(self.release['archives'])} archives · "
                                       + human_bytes(sum(row['bytes'] for row in self.release['archives'])))
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
            text = "Textures installed. You're ready to play."
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
        self.overall_active = False
        self.last_progress = self.last_overall_progress = 0
        self.overall_label.configure(text="")
        if kind in ("download", "update", "install"):
            self.progress.configure(mode="determinate", value=0, maximum=100)
            caption = "Overall install" if kind == "install" else "Overall download"
            self.overall_label.configure(text=caption + ": preparing…")
        if kind in ("download", "update"):
            self.pause.clear()
            self.progress.configure(mode="determinate", value=0, maximum=100)
            self.pause_button.configure(text="Pause", state="normal")
            self.pause_button.grid()
        elif kind != "install":
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

    def report_overall(self, message, done, total, completed, files):
        now = time.monotonic()
        if now - self.last_overall_progress > .1 or completed == files or done == 0:
            self.events.put(("overall-progress", message, done, total, completed, files))
            self.last_overall_progress = now

    def finish_progress(self, kind):
        self.progress.configure(value=100)
        caption = "Overall install" if kind == "install" else "Overall download"
        rows = self.release["archives"]
        self.overall_label.configure(text=f"{caption}: 100% · {len(rows)}/{len(rows)} PAKs · {human_bytes(sum(row['bytes'] for row in rows))}")

    def inspect(self, game, prepared=None, verify=False):
        if not game:
            return {}, None
        core.validate_game(game)
        status = self.core.status(game, verify=True) if verify else self.core.status(game)
        return status, self.core.preflight(prepared, game) if prepared else None

    def choose_game(self):
        folder = filedialog.askdirectory(title="Choose the folder containing EverQuest2.exe and paks", initialdir=self.game.get() or None)
        if folder:
            self.game.set(folder)
            self.verify_installation(full=False)

    def verify_installation(self, full=True):
        game, prepared = self.game.get().strip(), self.prepared
        if not game:
            self.set_message("Choose your EverQuest II folder first.")
            return
        self.restore_saved_ready()
        prepared = self.prepared
        self.begin("verify", "Checking your game folder and installed textures…", lambda: self.inspect(game, prepared, verify=full))

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
            try:
                latest = fetch_latest(self.state_dir, minimum_version=minimum, pause=self.pause.is_set, current_manifest=self.release)
            except (OSError, ValueError):
                prepared = self.core.saved_ready(self.bundled_release) if pack_identity(self.release) == pack_identity(self.bundled_release) else None
                if prepared is None:
                    raise
                status, preflight = self.inspect(game, prepared)
                notify("Update check unavailable; reusing saved files matching this app's bundled manifest.", 1, 1)
                return self.bundled_release, prepared, status, preflight
            if latest.get("schema") == "texturequest-release-v3":
                prepared = prepare_direct(self.core, latest, notify, self.pause.is_set,
                                          cache_in_use=self.cache_in_use, overall_progress=self.report_overall)
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
            return self.core.install(prepared, game, progress=self.report, overall_progress=self.report_overall)
        self.begin("install", "Installing TextureQuest. Keep the game and launcher closed until copying finishes…",
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
                    if not self.overall_active:
                        self.progress.configure(value=100 * done / max(total, 1))
                    if total > 1:
                        self.download_label.configure(text=f"Current file: {100 * done / total:.1f}% · {human_bytes(done)} of {human_bytes(total)}")
                    self.set_message(message)
                    continue
                if event[0] == "overall-progress":
                    _, message, done, total, completed, files = event
                    self.overall_active = True
                    # Commit/final preparation may still fail after the last byte.
                    percent = min(100 * done / max(total, 1), 99.9)
                    self.progress.configure(mode="determinate", value=percent)
                    caption = "Overall install" if self.busy == "install" else "Overall download"
                    self.overall_label.configure(text=f"{caption}: {percent:.1f}% · {completed}/{files} PAKs · {human_bytes(done)} of {human_bytes(total)}")
                    self.set_message(message)
                    continue
                event_type, kind, value = event
                self.busy = None
                saved_progress = float(self.progress.cget("value"))
                self.progress.stop()
                self.progress.configure(mode="determinate", maximum=100,
                                        value=saved_progress if kind in ("download", "update", "install") else 0)
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
                    self.finish_progress("download")
                elif kind == "check":
                    self.update_check = value
                    pack = value["pack"]
                    if pack.get("status") == "verified":
                        if pack_identity(pack["manifest"]) != pack_identity(self.release):
                            self.prepared = self.preflight = None
                        self.release = pack["manifest"]
                        self.release_checked = True
                        self.remember_pack_version()
                    else:
                        self.restore_saved_ready()
                    failures = [channel["message"] for channel in (value["pack"], value["installer"]) if channel.get("status") == "unavailable"]
                    self.set_message(" ".join(dict.fromkeys(failures)) if failures else
                                     "Version checks complete. App updates open on GitHub; texture downloads are prepared here.", error=bool(failures))
                    if self.release_checked and self.game.get().strip():
                        if self.initial_pack:
                            initial, self.initial_pack = self.initial_pack, None
                            self.prepare_zip(initial)
                        else:
                            self.verify_installation(full=False)
                    elif self.release_checked and self.initial_pack:
                        initial, self.initial_pack = self.initial_pack, None
                        self.prepare_zip(initial)
                elif kind in ("verify", "recover"):
                    self.installed, self.preflight = value
                    self.set_message(self.recover_message if kind == "recover" else installation_summary(self.installed, self.release), error=kind == "recover")
                elif event_type == "done" and kind == "install":
                    self.installed = value
                    self.remember_pack_version()
                    self.set_message("TextureQuest is ready. After a game patch, rerun this installer to reapply the textures.")
                    self.finish_progress("install")
                    self.show_install_success()
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
        root.update_idletasks()
        root.withdraw()
        def smoke():
            root.update_idletasks()
            total = sum(row['bytes'] for row in app.release['archives'])
            files = len(app.release['archives'])
            app.busy = "update"
            app.events.put(("overall-progress", "Downloading fixture.vpk", total // 2, total, files // 2, files))
            app.events.put(("download-progress", "Current file commit", 0, 1))
            app.poll()
            overall_download = float(app.progress.cget("value"))
            assert 49.9 < overall_download < 50.1 and "Overall download" in app.overall_label.cget("text")
            app.busy = "install"
            app.events.put(("overall-progress", "Installing fixture.vpk", total // 4, total, files // 4, files))
            app.poll()
            overall_install = float(app.progress.cget("value"))
            assert 24.9 < overall_install < 25.1 and "Overall install" in app.overall_label.cget("text")
            assert str(app.progress.cget("mode")) == "determinate"
            app.busy = None
            receipt = {"version": app.app_version, "texture_version": texture_version(app.release),
                       "app_update_status": app.app_update_label.cget("text"), "texture_update_status": app.pack_update_label.cget("text"),
                       "title": root.title(), "game": app.game.get(),
                       "install_enabled": app.install_button.cget("state") != "disabled", "download_button": app.download_button.cget("text"),
                       "width": root.winfo_width(), "height": root.winfo_height(), "release_sha256": core._digest(app.release),
                       "scrollable_body": False,
                       "overall_download_progress": overall_download, "overall_install_progress": overall_install,
                       "overall_progress_label": app.overall_label.cget("text"), "current_file_keeps_overall": True,
                       "launch_button": app.launch_button.cget("text"),
                       "launch_enabled": app.launch_button.cget("state") != "disabled",
                       "sideload_enabled": False,
                       "game_files_touched": False, "interface": "texturequest-compact-launcher", "explorer": False}
            (app.state_dir / "ui-smoke.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
            root.destroy()
        root.after(1000, smoke)
    root.mainloop()


if __name__ == "__main__":
    main()
