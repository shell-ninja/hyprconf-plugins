#!/usr/bin/env python3
"""
downloader.py — Hyprconf Media Downloader Backend Engine
Powered by yt-dlp & ffmpeg.
Provides streaming progress reporting for Dynamic Island and Top Bar Quickshell components.
"""

import sys
import os
import re
import json
import time
import argparse
import subprocess
from pathlib import Path

# Add hyprconf venv to sys.path to access yt-dlp
VENV_DIR = Path.home() / ".local" / "share" / "hyprconf" / "venv"
if VENV_DIR.exists():
    for p in (VENV_DIR / "lib").glob("python*/site-packages"):
        if str(p) not in sys.path:
            sys.path.insert(0, str(p))

try:
    import yt_dlp
except ImportError:
    yt_dlp = None

HOME = Path(os.environ.get("HOME", str(Path.home())))
LOCAL_HYPRCONF = HOME / ".local" / "share" / "hyprconf"
PLUGIN_DIR = LOCAL_HYPRCONF / "plugins" / "media-downloader"
CONFIG_FILE = PLUGIN_DIR / "config.json"
HISTORY_FILE = PLUGIN_DIR / "history.json"
CACHE_DIR = LOCAL_HYPRCONF / ".cache"
STATE_FILE = CACHE_DIR / "media_downloader_state.json"
PID_FILE = CACHE_DIR / "media_downloader.pid"


def load_config() -> dict:
    defaults = {
        "enabled": True,
        "download_dir": str(HOME / "Downloads"),
        "audio_dir": str(HOME / "Music"),
        "default_mode": "video",
        "video_quality": "best",
        "audio_format": "mp3",
        "auto_clipboard": False,
        "notify_on_complete": True
    }
    if CONFIG_FILE.exists():
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                user_cfg = json.load(f)
                defaults.update(user_cfg)
        except Exception:
            pass
    return defaults


def emit_state(data: dict):
    """Write atomic state JSON and stream to stdout."""
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    try:
        # Atomic file write
        temp_file = STATE_FILE.with_suffix(".tmp")
        with open(temp_file, "w", encoding="utf-8") as f:
            json.dump(data, f)
        temp_file.replace(STATE_FILE)
    except Exception:
        pass

    # Print to stdout for QML Process listening
    print(json.dumps(data), flush=True)


def load_history() -> list:
    if HISTORY_FILE.exists():
        try:
            with open(HISTORY_FILE, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            pass
    return []


def save_history(entry: dict):
    history = load_history()
    # Prepend new entry, limit to 50
    history = [entry] + [h for h in history if h.get("url") != entry.get("url")][:49]
    PLUGIN_DIR.mkdir(parents=True, exist_ok=True)
    try:
        with open(HISTORY_FILE, "w", encoding="utf-8") as f:
            json.dump(history, f, indent=2)
    except Exception:
        pass


def sanitize_filename(name: str) -> str:
    return re.sub(r'[\\/*?:"<>|]', "", name).strip()


def get_info(url: str):
    """Fast probe of video metadata without downloading."""
    if not yt_dlp:
        emit_state({"status": "error", "error": "yt-dlp is not installed in hyprconf environment."})
        return

    ydl_opts = {
        "quiet": True,
        "no_warnings": True,
        "skip_download": True,
        "extract_flat": False,
    }

    try:
        emit_state({"status": "fetching_info", "url": url})
        with yt_dlp.YoutubeDL(ydl_opts) as ydl:
            info = ydl.extract_info(url, download=False)
            if not info:
                emit_state({"status": "error", "error": "Could not retrieve media metadata."})
                return

            # Best effort resolution extraction
            title = info.get("title", "Unknown Title")
            duration = info.get("duration", 0)
            thumbnail = info.get("thumbnail", "")
            uploader = info.get("uploader", info.get("channel", "Unknown Artist/Uploader"))

            formats = []
            for f in info.get("formats", []):
                h = f.get("height")
                if h and f.get("vcodec") != "none":
                    if f"{h}p" not in formats:
                        formats.append(f"{h}p")

            result = {
                "status": "info_ready",
                "url": url,
                "title": title,
                "uploader": uploader,
                "duration": duration,
                "duration_str": time.strftime("%H:%M:%S" if duration >= 3600 else "%M:%S", time.gmtime(duration)) if duration else "--:--",
                "thumbnail": thumbnail,
                "formats": sorted(formats, key=lambda x: int(x.replace("p", "")), reverse=True)[:5],
            }
            emit_state(result)
            return result
    except Exception as e:
        emit_state({"status": "error", "error": str(e), "url": url})


def run_download(url: str, mode="video", quality="best", audio_format="mp3", out_dir=None):
    """Execute video or audio download with streaming progress."""
    if not yt_dlp:
        emit_state({"status": "error", "error": "yt-dlp is not installed in hyprconf environment."})
        return

    config = load_config()
    if not out_dir:
        out_dir = Path(config.get("audio_dir" if mode == "audio" else "download_dir")).expanduser()
    else:
        out_dir = Path(out_dir).expanduser()

    out_dir.mkdir(parents=True, exist_ok=True)

    # Save PID so cancel can kill it cleanly
    PID_FILE.write_text(str(os.getpid()))

    current_meta = {"title": "Downloading media...", "thumbnail": "", "uploader": ""}

    def progress_hook(d):
        status = d.get("status")
        if status == "downloading":
            total = d.get("total_bytes") or d.get("total_bytes_estimate") or 0
            downloaded = d.get("downloaded_bytes", 0)
            percent = (downloaded / total * 100.0) if total > 0 else 0.0

            # Raw speed string or formatted
            speed = d.get("speed")
            speed_str = f"{speed / (1024 * 1024):.1f} MB/s" if speed else d.get("_speed_str", "-- MB/s")

            eta = d.get("eta")
            eta_str = f"{eta // 60:02d}:{eta % 60:02d}" if eta is not None else d.get("_eta_str", "--:--")

            emit_state({
                "status": "downloading",
                "mode": mode,
                "url": url,
                "title": current_meta.get("title", "Downloading..."),
                "thumbnail": current_meta.get("thumbnail", ""),
                "percent": round(percent, 1),
                "speed": speed_str,
                "eta": eta_str,
                "downloaded_bytes": downloaded,
                "total_bytes": total
            })
        elif status == "finished":
            emit_state({
                "status": "processing",
                "mode": mode,
                "url": url,
                "title": current_meta.get("title", "Processing media..."),
                "thumbnail": current_meta.get("thumbnail", ""),
                "percent": 100.0,
                "step": "Converting / Merging streams with ffmpeg..."
            })

    ydl_opts = {
        "outtmpl": str(out_dir / "%(title)s.%(ext)s"),
        "progress_hooks": [progress_hook],
        "quiet": True,
        "no_warnings": True,
        "nocheckcertificate": True,
    }

    if mode == "audio":
        ydl_opts.update({
            "format": "bestaudio/best",
            "postprocessors": [
                {
                    "key": "FFmpegExtractAudio",
                    "preferredcodec": audio_format,
                    "preferredquality": "320" if audio_format == "mp3" else "0",
                },
                {
                    "key": "FFmpegMetadata",
                    "add_metadata": True,
                }
            ],
        })
    else:
        # Video Mode
        if quality == "1080p":
            ydl_opts["format"] = "bestvideo[height<=1080][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=1080]+bestaudio/best[height<=1080]/best"
        elif quality == "720p":
            ydl_opts["format"] = "bestvideo[height<=720][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=720]+bestaudio/best[height<=720]/best"
        else:
            ydl_opts["format"] = "bestvideo[ext=mp4]+bestaudio[ext=m4a]/bestvideo+bestaudio/best"
        ydl_opts["merge_output_format"] = "mp4"

    try:
        emit_state({"status": "starting", "url": url, "mode": mode})

        with yt_dlp.YoutubeDL(ydl_opts) as ydl:
            # First extract info to get title and thumbnail
            info = ydl.extract_info(url, download=False)
            if info:
                current_meta["title"] = info.get("title", "Media File")
                current_meta["thumbnail"] = info.get("thumbnail", "")
                current_meta["uploader"] = info.get("uploader", "")

            # Perform the actual download
            download_ret = ydl.extract_info(url, download=True)
            target_filename = ydl.prepare_filename(download_ret) if download_ret else ""
            if mode == "audio":
                target_filename = str(Path(target_filename).with_suffix(f".{audio_format}"))
            elif mode == "video" and not target_filename.endswith(".mp4"):
                target_filename = str(Path(target_filename).with_suffix(".mp4"))

            complete_entry = {
                "status": "complete",
                "url": url,
                "mode": mode,
                "title": current_meta["title"],
                "uploader": current_meta["uploader"],
                "thumbnail": current_meta["thumbnail"],
                "filepath": target_filename,
                "timestamp": int(time.time()),
                "filename": Path(target_filename).name if target_filename else "downloaded_media"
            }

            emit_state(complete_entry)
            save_history(complete_entry)

            # Send desktop notification
            if config.get("notify_on_complete", True):
                try:
                    subprocess.run([
                        "notify-send",
                        "-a", "Hyprconf Media Downloader",
                        "-i", "download",
                        "Download Complete",
                        f"{current_meta['title']}\nSaved to {out_dir.name}/"
                    ], check=False)
                except Exception:
                    pass

    except Exception as e:
        emit_state({"status": "error", "error": str(e), "url": url})
    finally:
        if PID_FILE.exists():
            try:
                PID_FILE.unlink()
            except Exception:
                pass


def cancel_download():
    """Cancel active download process."""
    if PID_FILE.exists():
        try:
            pid = int(PID_FILE.read_text().strip())
            os.kill(pid, 15)  # SIGTERM
            time.sleep(0.3)
            os.kill(pid, 9)   # SIGKILL
        except Exception:
            pass
        finally:
            try:
                PID_FILE.unlink()
            except Exception:
                pass
    emit_state({"status": "idle", "message": "Download cancelled."})


def check_clipboard() -> dict:
    """Check wl-paste for supported media URLs."""
    try:
        text = subprocess.check_output(["wl-paste", "--no-newline"], text=True, timeout=1).strip()
    except Exception:
        return {"status": "none"}

    media_patterns = [
        r'https?://(?:www\.)?(?:youtube\.com|youtu\.be)/',
        r'https?://(?:www\.)?instagram\.com/(?:reel|p|tv)/',
        r'https?://(?:www\.)?reddit\.com/r/[^/]+/comments/',
        r'https?://v\.redd\.it/',
        r'https?://(?:www\.)?(?:twitter\.com|x\.com)/[^/]+/status/',
        r'https?://(?:www\.)?tiktok\.com/@',
        r'https?://(?:www\.)?facebook\.com/',
        r'https?://(?:www\.)?fb\.watch/',
        r'https?://soundcloud\.com/',
        r'https?://(?:www\.)?vimeo\.com/'
    ]

    is_media = any(re.search(pat, text) for pat in media_patterns)
    if is_media:
        return {"status": "clipboard_match", "url": text}
    return {"status": "none", "clipboard": text[:40]}


def main():
    parser = argparse.ArgumentParser(description="Hyprconf Media Downloader CLI & Engine")
    sub = parser.add_subparsers(dest="command")

    # info
    p_info = sub.add_parser("info", help="Get media metadata")
    p_info.add_argument("url", help="Media URL")

    # download
    p_dl = sub.add_parser("download", help="Download media")
    p_dl.add_argument("url", help="Media URL")
    p_dl.add_argument("--mode", choices=["video", "audio"], default="video")
    p_dl.add_argument("--quality", choices=["best", "1080p", "720p"], default="best")
    p_dl.add_argument("--audio-format", choices=["mp3", "m4a", "flac"], default="mp3")
    p_dl.add_argument("--outdir", default=None)

    # cancel
    sub.add_parser("cancel", help="Cancel active download")

    # history
    sub.add_parser("history", help="List recent downloads")

    # clipboard
    sub.add_parser("clipboard", help="Check clipboard for media link")

    # status
    sub.add_parser("status", help="Get current status")

    args = parser.parse_args()

    if args.command == "info":
        get_info(args.url)
    elif args.command == "download":
        run_download(args.url, args.mode, args.quality, args.audio_format, args.outdir)
    elif args.command == "cancel":
        cancel_download()
    elif args.command == "history":
        print(json.dumps(load_history(), indent=2))
    elif args.command == "clipboard":
        print(json.dumps(check_clipboard(), indent=2))
    elif args.command == "status":
        if STATE_FILE.exists():
            try:
                print(STATE_FILE.read_text())
                return
            except Exception:
                pass
        print(json.dumps({"status": "idle"}))
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
