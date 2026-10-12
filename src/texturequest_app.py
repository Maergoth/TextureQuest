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

VERSION = "1.0.0"
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
    def __init__(self, root, state_dir=None, initial_game=None, initial_pack=None):
        self.root = root
        self.state_dir = core._plain(state_dir or state_directory())
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.core = core.Swapper(self.state_dir)
        self.release = load_manifest(resource("release-manifest.json"))
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
        root.geometry("840x760")
        root.minsize(760, 760)
        root.configure(bg=BG)
        root.option_add("*Font", ("Segoe UI", 10))
        self.build()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(80, self.poll)
        if initial_pack:
            root.after(200, lambda: self.prepare_zip(initial_pack))
        elif initial_game:
            root.after(200, self.verify_installation)

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
        self.label(header, "v1.0", color=GOLD).grid(row=0, column=1, sticky="ne", padx=(18, 0))
        body = tk.Frame(self.root, bg=BG, padx=24, pady=16)
        body.grid(row=1, column=0, sticky="nsew")
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
        self.label(pack_card, "23,277 textures · 27.8 GB download · 1K / 2K detail", color=MUTED).grid(row=1, column=0, sticky="w", pady=(4, 9))
        row = tk.Frame(pack_card, bg=WHITE)
        row.grid(row=2, column=0, sticky="ew")
        row.columnconfigure(1, weight=1)
        self.download_button = self.button(row, "Download pack", self.download_pack)
        self.download_button.grid(row=0, column=0, sticky="w")
        self.download_label = self.label(row, "Not downloaded", color=MUTED, wraplength=350, justify="left")
        self.download_label.grid(row=0, column=1, sticky="w", padx=(14, 0))
        self.label(pack_card, "Allow 56 GB for download/cache, plus 16 GB free on the game drive.", size=9, color=MUTED).grid(row=3, column=0, sticky="w", pady=(9, 0))
        install_card = self.card(body, 2, "3", "Install TextureQuest")
        self.install_label = self.label(install_card, "Choose a game folder and prepare the download first.", color=MUTED, wraplength=610, justify="left")
        self.install_label.grid(row=1, column=0, sticky="w", pady=(4, 10))
        self.install_button = self.button(install_card, "Install TextureQuest", self.install, primary=True)
        self.install_button.grid(row=2, column=0, sticky="w")
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
        self.label(footer, "After patching, reinstall. For ordinary play, use EverQuest2.exe.", size=9, color=MUTED).grid(row=0, column=0, sticky="w")
        self.menu_button = tk.Menubutton(footer, text="Maintenance & updates ▾", bg=BG, fg=GREEN, activebackground="#e8ede6", relief="flat", cursor="hand2", padx=8, pady=6)
        self.menu_button.grid(row=1, column=0, sticky="w", pady=(5, 0))
        self.menu = tk.Menu(self.menu_button, tearoff=False, bg=WHITE, fg=INK)
        self.menu_button.configure(menu=self.menu)
        self.menu.add_command(label="Verify installed textures", command=self.verify_installation)
        self.menu.add_command(label="Prepare for a game patch", command=lambda: self.unlock("patch"))
        self.menu.add_command(label="Return to original textures", command=lambda: self.unlock("stock"))
        self.menu.add_separator()
        self.menu.add_command(label="Use an existing Texture-Pack.zip…", command=self.choose_zip)
        self.menu.add_command(label="Clear downloaded files & cache…", command=self.clear_cache)
        self.menu.add_separator()
        self.menu.add_command(label="Check for updates", command=lambda: webbrowser.open(UPDATES))
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
        self.install_button.configure(state="normal" if can_install(self.busy, self.prepared, self.preflight) else "disabled")
        current = installation_current(self.installed, self.release)
        self.install_button.configure(text="Reinstall TextureQuest" if current else "Install TextureQuest")
        if self.prepared:
            self.download_label.configure(text="Downloaded and verified")
            self.download_button.configure(text="Verify saved pack")
        else:
            folder = self.state_dir / "downloads"
            if self.saved_zip_path() or (folder / "Texture-Pack.zip").is_file():
                caption, state = "Prepare saved pack", "Saved download available"
            elif (folder / "Texture-Pack.zip.download").is_file():
                caption, state = "Resume download", "Download paused or interrupted"
            else:
                caption, state = "Download pack", "Not downloaded"
            self.download_label.configure(text=state)
            self.download_button.configure(text=caption)
        if current:
            text = "Installed and verified. Launch EverQuest2.exe directly to play."
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
        for index in (1, 2):
            self.menu.entryconfigure(index, state="normal" if self.installed.get("managed_count") else "disabled")

    def begin(self, kind, message, action):
        if self.busy:
            return
        self.busy = kind
        self.set_message(message)
        self.refresh_controls()
        if kind == "download":
            self.pause.clear()
            self.progress.configure(mode="determinate", value=0, maximum=self.release["archive"]["bytes"])
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

    def choose_zip(self):
        path = filedialog.askopenfilename(title="Choose TextureQuest 1.0.0 Texture-Pack.zip", filetypes=[("TextureQuest ZIP", "*.zip")])
        if path:
            self.prepare_zip(path)

    def prepare_zip(self, path):
        game = self.game.get().strip()
        def work():
            prepared = prepare_release(self.core, path, self.release, self.report)
            status, preflight = self.inspect(game, prepared)
            return prepared, status, preflight
        self.begin("prepare", "Checking the complete pack and preparing it for installation. This may take a few minutes…", work)

    def download_pack(self):
        saved = self.saved_zip_path()
        if saved:
            self.prepare_zip(saved)
            return
        def notify(message, done, total):
            now = time.monotonic()
            if now - self.last_progress > .12 or done == total:
                self.events.put(("download-progress", message, done, total))
                self.last_progress = now
        def work():
            with core._lock(self.state_dir / "operation.lock"):
                folder = core._plain(self.state_dir / "downloads", self.state_dir)
                needed = download_space_needed(self.release, folder, (self.state_dir / "texturequest-prepared.json").exists())
                if shutil.disk_usage(self.state_dir).free < needed:
                    raise OSError(f"Free at least {human_bytes(needed)} on this app's drive to download and prepare the pack. You can move the app to a larger drive.")
                return download(self.release, folder, notify, self.pause.is_set)
        self.begin("download", "Connecting to the TextureQuest release…", work)

    def request_pause(self):
        if self.busy == "download":
            self.pause.set()
            self.pause_button.configure(text="Pausing…", state="disabled")
            self.set_message("Pausing safely. Downloaded data will be kept.")

    def install(self):
        if not can_install(self.busy, self.prepared, self.preflight):
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

    def unlock(self, purpose):
        game = self.game.get().strip()
        def work():
            self.core.uninstall(game, progress=self.report)
            return purpose, self.core.status(game)
        self.begin("unlock", "Removing TextureQuest's file protection…", work)

    def clear_cache(self):
        if self.busy or not messagebox.askyesno("Clear downloaded files", "Remove this app's downloaded ZIP and prepared copies?\n\nInstalled game textures and installation records stay intact. Reinstalling will need a new download or a saved ZIP.", parent=self.root):
            return
        def work():
            with core._lock(self.state_dir / "operation.lock"):
                for name in ("downloads", "prepared", "explorer"):
                    target = core._plain(self.state_dir / name, self.state_dir)
                    if target.exists():
                        for entry in target.rglob("*"):
                            core._plain(entry, target)
                        shutil.rmtree(target)
                core._plain(self.state_dir / "texturequest-prepared.json", self.state_dir).unlink(missing_ok=True)
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
                    self.progress.configure(value=done)
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
                    if kind in ("install", "unlock") and self.game.get().strip():
                        self.recover_message = str(value)
                        game, prepared = self.game.get().strip(), self.prepared
                        self.begin("recover", str(value), lambda: self.inspect(game, prepared))
                elif kind == "download":
                    if not self.close_after_pause:
                        self.prepare_zip(value)
                elif kind == "prepare":
                    self.prepared, self.installed, self.preflight = value
                    self.set_message("Pack verified. Click Install when the game and launcher are closed.")
                elif kind in ("verify", "recover"):
                    self.installed, self.preflight = value
                    self.set_message(self.recover_message if kind == "recover" else installation_summary(self.installed, self.release), error=kind == "recover")
                elif kind == "install":
                    self.installed = value
                    self.set_message("TextureQuest is ready. Launch EverQuest2.exe directly to play. Reinstall after game patches.")
                    self.progress.configure(value=100)
                elif kind == "unlock":
                    purpose, self.installed = value
                    guidance = ("Files unlocked. Run the official EQ2 patcher to restore the original textures. Modified bytes remain until the patcher replaces them."
                                if purpose == "stock" else "Files unlocked. Run the official patcher, close it, then reinstall TextureQuest before playing.")
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
        if self.busy == "download":
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
        run(args.verify_runtime)
        return
    root = tk.Tk()
    app = TextureQuestApp(root, args.data_dir, args.game_dir, args.pack)
    if args.smoke_test:
        def smoke():
            root.update_idletasks()
            receipt = {"version": VERSION, "title": root.title(), "game": app.game.get(),
                       "install_enabled": app.install_button.cget("state") != "disabled", "download_button": app.download_button.cget("text"),
                       "width": root.winfo_width(), "height": root.winfo_height(), "release_sha256": app.release["archive"]["sha256"],
                       "game_files_touched": False, "interface": "texturequest-three-step-consumer", "explorer": False}
            (app.state_dir / "ui-smoke.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
            root.destroy()
        root.after(1000, smoke)
    root.mainloop()


if __name__ == "__main__":
    main()
