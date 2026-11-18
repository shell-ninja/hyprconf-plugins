# Media Downloader for Hyprconf

A universal media downloader and audio extractor designed natively for the **Hyprconf** desktop environment.

---

## Features

- **Dynamic Island Integration**: Full notch expansion with live speed (MB/s), ETA, and thumbnail preview.
- **Top Bar Integration**: Live animated progress pill and quick popup downloader.
- **1,800+ Supported Sites**: Powered by `yt-dlp` and `ffmpeg` (YouTube, Instagram, Reddit, Facebook, TikTok, X, SoundCloud, etc.).
- **Smart Audio Extraction**: High-bitrate MP3 / M4A / FLAC with metadata.
- **Obsidian Glass Aesthetics**: Native Qt6 Quickshell styling with wallpaper-derived accents.

---

## Usage

- **Dynamic Island**: Click the Media Downloader icon in the Island Control Center or press `Super + Shift + D`.
- **Top Bar**: Click the Media Downloader icon in the top panel.
- **CLI**:
  ```bash
  python3 bin/downloader.py download "<URL>"
  python3 bin/downloader.py download "<URL>" --mode audio --audio-format mp3
  ```
