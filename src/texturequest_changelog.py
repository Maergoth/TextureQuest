"""Bounded bundled changelog text and its read-only, nonmodal Tk viewer."""
from __future__ import annotations
import hashlib
from pathlib import Path
import re
import tkinter as tk
from tkinter import ttk
import webbrowser

MAX_BYTES = 512 * 1024
DEVELOP_URL = "https://github.com/Maergoth/TextureQuest/blob/Develop/CHANGELOG.md"


def validate_changelog(raw: bytes, version: str) -> str:
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Invalid changelog application version")
    if not raw or len(raw) > MAX_BYTES:
        raise ValueError("Bundled changelog is empty or too large")
    text = raw.decode("utf-8-sig")
    if any(ord(char) < 32 and char not in "\r\n\t" for char in text):
        raise ValueError("Bundled changelog contains unsupported control characters")
    if not re.search(r"(?m)^##[ \t]+" + re.escape(version) + r"(?=[ \t\r\n]|$)", text):
        raise ValueError("CHANGELOG.md has no entry for App " + version)
    return text


def load_changelog(path: Path, version: str, expected_sha256: str | None = None) -> tuple[str, str]:
    with Path(path).open("rb") as stream:
        raw = stream.read(MAX_BYTES + 1)
    digest = hashlib.sha256(raw).hexdigest()
    if expected_sha256 is not None and digest != expected_sha256:
        raise ValueError("Bundled changelog differs from its build pin")
    return validate_changelog(raw, version), digest


def target_release(update: dict | None) -> tuple[str, str] | None:
    if not isinstance(update, dict):
        return None
    update = update or {}
    version, url = update.get("version"), update.get("release_url")
    if (update.get("status") == "verified" and update.get("update_available") is True
            and isinstance(version, str) and re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version)
            and isinstance(url, str) and re.fullmatch(
                r"https://github\.com/Maergoth/TextureQuest/releases/tag/[A-Za-z0-9._~-]+", url)):
        return version, url
    return None


def show_changelog(parent, version: str, text: str, update: dict | None = None):
    """Display plain text only. Links open solely after an explicit click."""
    dialog = tk.Toplevel(parent)
    dialog.title("What's new — App " + version + " (installed)")
    dialog.transient(parent)
    dialog.geometry("760x570")
    dialog.minsize(480, 320)
    dialog.configure(bg="#10171e")
    dialog.columnconfigure(0, weight=1)
    dialog.rowconfigure(2, weight=1)
    caption = tk.Label(dialog, text="Installed App " + version + " · bundled changelog and history",
                       bg="#10171e", fg="#edf3f6", anchor="w", font=("Segoe UI", 11, "bold"))
    caption.grid(row=0, column=0, sticky="ew", padx=16, pady=(14, 6))
    target = target_release(update)
    notice = ("App " + target[0] + " is available. These bundled notes describe the installed app; "
              "open the update's release notes below." if target else
              "These notes ship with this app. The Develop changelog may include newer work.")
    explanation = tk.Label(dialog, text=notice, bg="#10171e", fg="#99aab7", anchor="w", justify="left",
                           wraplength=700)
    explanation.grid(row=1, column=0, sticky="ew", padx=16, pady=(0, 10))
    explanation.bind("<Configure>", lambda event: explanation.configure(wraplength=max(240, event.width)))
    body = tk.Frame(dialog, bg="#10171e")
    body.grid(row=2, column=0, sticky="nsew", padx=16)
    body.columnconfigure(0, weight=1)
    body.rowconfigure(0, weight=1)
    content = tk.Text(body, wrap="word", font=("Segoe UI", 10), bg="#19242e", fg="#edf3f6",
                      relief="flat", padx=12, pady=10, selectbackground="#354e60")
    scrollbar = ttk.Scrollbar(body, orient="vertical", command=content.yview)
    content.configure(yscrollcommand=scrollbar.set)
    content.grid(row=0, column=0, sticky="nsew")
    scrollbar.grid(row=0, column=1, sticky="ns")
    content.insert("1.0", text)
    content.configure(state="disabled")
    links = tk.Frame(dialog, bg="#10171e")
    links.grid(row=3, column=0, sticky="ew", padx=16, pady=12)
    links.columnconfigure(0, weight=1)
    ttk.Button(links, text="Develop changelog on GitHub", command=lambda: webbrowser.open(DEVELOP_URL)).grid(row=0, column=0, sticky="w")
    if target:
        ttk.Button(links, text="App " + target[0] + " release notes",
                   command=lambda: webbrowser.open(target[1])).grid(row=1, column=0, sticky="w", pady=(6, 0))
    ttk.Button(links, text="Close", command=dialog.destroy).grid(row=0, column=1, rowspan=2, sticky="e", padx=(8, 0))
    dialog.bind("<Escape>", lambda _: dialog.destroy())
    dialog.changelog_text = content
    dialog.changelog_scrollbar = scrollbar
    dialog.changelog_caption = caption
    return dialog
