#!/usr/bin/env python3
"""
downloader.py — Hyprconf Media Downloader Backend Engine
Powered by yt-dlp & ffmpeg.
Provides streaming JSON progress reporting for Dynamic Island and Top Bar Quickshell components.
"""

import sys
import os
import re
import json
import time
import argparse
import subprocess
import urllib.request
from pathlib import Path

# Add hyprconf venv to sys.path to access yt-dlp and ffmpeg dependencies
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


class QuietLogger:
    """Suppresses yt-dlp internal stdout logging so only atomic JSON is emitted."""
    def debug(self, msg): pass
    def info(self, msg): pass
    def warning(self, msg): pass
    def error(self, msg): pass


def load_config() -> dict:
    defaults = {
        "enabled": True,
        "download_dir": str(HOME / "Downloads"),
        "audio_dir": str(HOME / "Music"),
        "default_mode": "video",
        "video_quality": "best",
        "audio_format": "mp3",
        "auto_clipboard": False,
        "notify_on_complete": True,
        "browser_cookies": "auto"
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
        temp_file = STATE_FILE.with_suffix(".tmp")
        with open(temp_file, "w", encoding="utf-8") as f:
            json.dump(data, f)
        temp_file.replace(STATE_FILE)
    except Exception:
        pass

    # Print pure JSON to stdout for QML Process SplitParser listening
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


def resolve_canonical_url(url: str) -> str:
    """Follow HTTP 30x redirects to unshorten URLs (e.g. youtu.be, redd.it, reddit.com/r/.../s/..., v.redd.it)."""
    url = url.strip()
    if not (url.startswith("http://") or url.startswith("https://")):
        return url

    try:
        req = urllib.request.Request(
            url,
            headers={
                "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
                "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
            }
        )
        with urllib.request.urlopen(req, timeout=4) as resp:
            final_url = resp.geturl()
            if final_url:
                return final_url
    except Exception:
        pass
    return url


def discover_browser_cookies(browser_pref="auto"):
    """Auto-detect browser cookies across Brave (including Brave-Origin), Chrome, Chromium, Firefox."""
    config_dir = HOME / ".config"

    # Precedence order of candidates
    candidates = [
        ("brave", config_dir / "BraveSoftware" / "Brave-Origin" / "Default"),
        ("brave", config_dir / "BraveSoftware" / "Brave-Origin"),
        ("brave", config_dir / "BraveSoftware" / "Brave-Browser" / "Default"),
        ("brave", config_dir / "BraveSoftware" / "Brave-Browser"),
        ("chrome", config_dir / "google-chrome" / "Default"),
        ("chrome", config_dir / "google-chrome"),
        ("chromium", config_dir / "chromium" / "Default"),
        ("chromium", config_dir / "chromium"),
        ("firefox", HOME / ".mozilla" / "firefox"),
    ]

    if browser_pref and browser_pref != "auto":
        candidates = [c for c in candidates if c[0] == browser_pref.lower()]

    for browser_name, path in candidates:
        if path.exists():
            if (path / "Cookies").exists() or (path / "Network" / "Cookies").exists() or browser_name == "firefox":
                return (browser_name, str(path))
            if path.is_dir() and not (path / "Default").exists():
                return (browser_name, str(path))

    return None


def get_base_ydl_opts(extra_opts=None) -> dict:
    """Build robust yt-dlp options with quiet logging, node js runtime, browser impersonation and cookie discovery."""
    config = load_config()
    opts = {
        "quiet": True,
        "no_warnings": True,
        "noprogress": True,
        "nocheckcertificate": True,
        "logger": QuietLogger(),
        "js_runtimes": {"node": {}},
        "remote_components": ["ejs:github"]
    }

    # Browser impersonation (uses curl_cffi to match genuine Chrome TLS fingerprint, bypassing Akamai/Cloudflare/Reddit blocks)
    try:
        from yt_dlp.networking.impersonate import ImpersonateTarget
        opts["impersonate"] = ImpersonateTarget.from_str("chrome")
    except Exception:
        pass

    # Cookie configuration
    cookie_file = Path(config.get("cookie_file", "")) if config.get("cookie_file") else (LOCAL_HYPRCONF / "cookies.txt")
    if cookie_file.exists():
        opts["cookiefile"] = str(cookie_file)
    else:
        # Auto-detect browser cookies if configured
        browser_pref = config.get("browser_cookies", "auto")
        cookie_tuple = discover_browser_cookies(browser_pref)
        if cookie_tuple:
            opts["cookiesfrombrowser"] = cookie_tuple

    if extra_opts:
        opts.update(extra_opts)
    return opts


def clean_error_message(err_str: str, url: str) -> str:
    """Map raw yt-dlp error traces to clean, user-friendly error messages."""
    s = str(err_str)
    if "Account authentication is required" in s or ("403" in s and "reddit" in url.lower()):
        return "Reddit requires authentication (HTTP 403). Please log in to Reddit in your browser or save cookies to ~/.local/share/hyprconf/cookies.txt"
    if "Requested format is not available" in s:
        return "The requested video resolution is not available for this media."
    if "Video unavailable" in s or "Private video" in s:
        return "This media is private or unavailable."
    if "Sign in to confirm you’re not a bot" in s:
        return "Platform requested anti-bot verification. Try again in a moment."
    if "HTTP Error 429" in s:
        return "Rate limit reached. Please wait a moment before downloading again."
    # Remove verbose traceback or prefix
    s = re.sub(r"^ERROR:\s*\[[^\]]+\]\s*", "", s).strip()
    if len(s) > 180:
        s = s[:177] + "..."
    return s


def get_info(url: str):
    """Fast probe of video metadata without downloading."""
    if not yt_dlp:
        emit_state({"status": "error", "error": "yt-dlp is not installed in hyprconf environment."})
        return

    resolved_url = resolve_canonical_url(url)
    ydl_opts = get_base_ydl_opts({
        "skip_download": True,
        "extract_flat": False,
    })

    try:
        emit_state({"status": "fetching_info", "url": resolved_url})
        with yt_dlp.YoutubeDL(ydl_opts) as ydl:
            info = ydl.extract_info(resolved_url, download=False)
            if not info:
                emit_state({"status": "error", "error": "Could not retrieve media metadata."})
                return

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
                "url": resolved_url,
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
        emit_state({"status": "error", "error": clean_error_message(str(e), resolved_url), "url": resolved_url})


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
    PID_FILE.write_text(str(os.getpid()))

    # Clean up stale 0-byte .part files from previous abrupt terminations
    try:
        for f in out_dir.glob("*.part"):
            if f.stat().st_size == 0:
                f.unlink(missing_ok=True)
    except Exception:
        pass

    resolved_url = resolve_canonical_url(url)
    current_meta = {
        "title": "",
        "thumbnail": "",
        "uploader": ""
    }

    def progress_hook(d):
        status = d.get("status")
        if status == "downloading":
            total = d.get("total_bytes") or d.get("total_bytes_estimate") or 0
            downloaded = d.get("downloaded_bytes", 0)
            if total > 0:
                percent = downloaded / total * 100.0
            elif d.get("fragment_count"):
                percent = (
                    float(d.get("fragment_index", 0))
                    / float(d.get("fragment_count", 1))
                    * 100.0
                )
            else:
                percent = 0.0

            # Extract metadata from info_dict if available
            info_dict = d.get("info_dict") or {}
            if info_dict:
                if not current_meta.get("title") or current_meta["title"] == "Fetching media details...":
                    current_meta["title"] = info_dict.get("title", current_meta.get("title", "Fetching media details..."))
                if not current_meta.get("thumbnail"):
                    current_meta["thumbnail"] = info_dict.get("thumbnail", "")
                if not current_meta.get("uploader"):
                    current_meta["uploader"] = info_dict.get("uploader", "")

            # Speed formatting
            speed = d.get("speed")
            if speed is not None:
                try:
                    s_val = float(speed)
                    if s_val >= 1024 * 1024:
                        speed_str = f"{s_val / (1024 * 1024):.1f} MB/s"
                    elif s_val >= 1024:
                        speed_str = f"{s_val / 1024:.0f} KB/s"
                    else:
                        speed_str = f"{s_val:.0f} B/s"
                except Exception:
                    speed_str = str(d.get("_speed_str", "-- MB/s"))
            else:
                speed_str = str(d.get("_speed_str", "-- MB/s"))

            # ETA formatting - safely cast float to int before formatting
            eta = d.get("eta")
            if eta is not None:
                try:
                    eta_sec = int(float(eta))
                    eta_str = f"{eta_sec // 60:02d}:{eta_sec % 60:02d}"
                except Exception:
                    eta_str = str(d.get("_eta_str", "--:--"))
            else:
                eta_str = str(d.get("_eta_str", "--:--"))

            emit_state({
                "status": "downloading",
                "mode": mode,
                "url": resolved_url,
                "title": current_meta.get("title", "Downloading media..."),
                "thumbnail": current_meta.get("thumbnail", ""),
                "uploader": current_meta.get("uploader", ""),
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
                "url": resolved_url,
                "title": current_meta.get("title", "Processing media..."),
                "thumbnail": current_meta.get("thumbnail", ""),
                "uploader": current_meta.get("uploader", ""),
                "percent": 100.0,
                "step": "Converting / Merging streams with ffmpeg..."
            })

    extra_opts = {
        "outtmpl": str(out_dir / "%(title)s.%(ext)s"),
        "progress_hooks": [progress_hook],
    }

    if mode == "audio":
        extra_opts.update({
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
            extra_opts["format"] = "bestvideo[height<=1080]+bestaudio/best[height<=1080]/best"
        elif quality == "720p":
            extra_opts["format"] = "bestvideo[height<=720]+bestaudio/best[height<=720]/best"
        else:
            extra_opts["format"] = "bestvideo+bestaudio/best"
        extra_opts["merge_output_format"] = "mp4"

    ydl_opts = get_base_ydl_opts(extra_opts)

    try:
        emit_state({
            "status": "starting",
            "url": resolved_url,
            "mode": mode,
            "title": "",
            "thumbnail": "",
            "uploader": "",
            "percent": 0.0
        })

        with yt_dlp.YoutubeDL(ydl_opts) as ydl:
            download_ret = ydl.extract_info(resolved_url, download=True)
            if download_ret:
                current_meta["title"] = download_ret.get("title", current_meta.get("title", "Media File"))
                current_meta["thumbnail"] = download_ret.get("thumbnail", current_meta.get("thumbnail", ""))
                current_meta["uploader"] = download_ret.get("uploader", current_meta.get("uploader", ""))
                target_filename = ydl.prepare_filename(download_ret)
            else:
                target_filename = ""

            if mode == "audio":
                target_filename = str(Path(target_filename).with_suffix(f".{audio_format}"))
            elif mode == "video" and not target_filename.endswith(".mp4"):
                target_filename = str(Path(target_filename).with_suffix(".mp4"))

            complete_entry = {
                "status": "complete",
                "url": resolved_url,
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
        emit_state({"status": "error", "error": clean_error_message(str(e), resolved_url), "url": resolved_url})
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
        r'https?://(?:www\.)?reddit\.com/r/[^/]+/s/',
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
