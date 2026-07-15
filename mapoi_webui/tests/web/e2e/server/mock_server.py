#!/usr/bin/env python3
"""ROS 非依存のモック webui サーバ (mapoi_webui Playwright e2e 用, #284)。

実 `mapoi_webui/web/` を静的配信し、`/api/*` を `e2e/fixtures/state.json` 由来の
決定論的 JSON で返す。実 Flask backend (mapoi_webui_node.py) は import 時に
rclpy / mapoi_interfaces を要し CI が重く、フィクスチャも制御できないため再利用しない。
本サーバは Python 標準ライブラリのみ (CI に Python があれば追加依存ゼロ) で動く。

- 静的: `/` -> web/index.html, `/css/<f>` -> web/css/<f>, `/js/<f>` -> web/js/<f>,
        `/vendor/<f>` -> web/vendor/<f> (#394 同梱 Leaflet)
- API GET: /api/maps, /api/maps/<n>/metadata, /api/maps/<n>/image (動的生成 PNG),
           /api/pois, /api/routes, /api/tag-definitions, /api/nav/status, /api/mode
- API POST: /api/pois ほか -> {"success": true, ...} (テストは save を踏まないが契約上用意)
- SSE: /api/events -> text/event-stream を張りっぱなしにし keepalive (フロントの
       EventSource が onerror で再接続を繰り返さないように 200 を返し続ける)
- test 専用: POST /test/sse-event -> body の JSON を接続中の全 SSE client へ配信 (#384)。
       e2e から config_changed 等のイベント受信をシミュレートする (実 backend には無い)
"""
import argparse
import json
import queue
import re
import struct
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

SCRIPT_DIR = Path(__file__).resolve().parent
FIXTURE_PATH = SCRIPT_DIR.parent / "fixtures" / "state.json"


def find_web_dir():
    """`__file__` から上方向に web/js/app.js を持つディレクトリを探し web/ を返す。"""
    for parent in SCRIPT_DIR.parents:
        candidate = parent / "web"
        if (candidate / "js" / "app.js").is_file():
            return candidate
    raise RuntimeError("web/ (web/js/app.js) が見つからない: layout を確認")


WEB_DIR = find_web_dir()
with FIXTURE_PATH.open(encoding="utf-8") as f:
    STATE = json.load(f)

CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".png": "image/png",
    ".svg": "image/svg+xml",
    ".json": "application/json; charset=utf-8",
    ".ico": "image/x-icon",
}


def make_png(width, height):
    """zlib だけで 8bit グレースケール PNG を生成 (バイナリを git に置かないため動的生成)。

    Leaflet の imageOverlay は metadata の bounds に合わせて伸縮するので、画素内容は
    smoke の本質に無関係。視認用に薄いチェッカーを描く。"""
    def chunk(typ, data):
        return (
            struct.pack(">I", len(data))
            + typ
            + data
            + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)  # 8bit grayscale
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        for x in range(width):
            raw.append(210 if ((x // 20) + (y // 20)) % 2 == 0 else 170)
    idat = zlib.compress(bytes(raw), 6)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", idat)
        + chunk(b"IEND", b"")
    )


_meta = STATE["metadata"]
MAP_PNG = make_png(int(_meta["width"]), int(_meta["height"]))

# POST /test/sse-event で e2e からイベントを注入するための接続中 SSE client 一覧 (#384)。
# 実 backend (mapoi_webui_node) の _sse_clients と同じ queue fanout 構造の最小版。
SSE_CLIENTS = set()
SSE_LOCK = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # ノイズ抑制 (Playwright webServer 出力を汚さない)
        pass

    # ---- helpers ----
    def _send_bytes(self, status, body, content_type, extra_headers=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _send_json(self, obj, status=200):
        self._send_bytes(status, json.dumps(obj).encode("utf-8"),
                         CONTENT_TYPES[".json"])

    def _send_static(self, rel_path):
        target = (WEB_DIR / rel_path).resolve()
        # ディレクトリトラバーサル防止
        if WEB_DIR not in target.parents and target != WEB_DIR:
            self._send_json({"error": "forbidden"}, 403)
            return
        if not target.is_file():
            self._send_json({"error": "not found", "path": rel_path}, 404)
            return
        ctype = CONTENT_TYPES.get(target.suffix, "application/octet-stream")
        self._send_bytes(200, target.read_bytes(), ctype)

    def _send_sse(self):
        # フロントの EventSource('/api/events') 用。200 + text/event-stream を張りっぱなしに
        # して onerror による再接続ループ・404 を避ける。daemon thread なので process 終了で死ぬ。
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        if self.command == "HEAD":
            return  # HEAD は header のみ。stream loop に入らない (無限ループ回避)。
        # /test/sse-event からの注入を受け取る client queue を登録 (#384)。イベントが無い間は
        # 従来どおり 1 秒間隔の keepalive comment を流す (queue.get の timeout で兼ねる)。
        q = queue.Queue(maxsize=10)
        with SSE_LOCK:
            SSE_CLIENTS.add(q)
        try:
            self.wfile.write(b": connected\n\n")
            self.wfile.flush()
            while True:
                try:
                    event = q.get(timeout=1.0)
                    body = json.dumps(event).encode("utf-8")
                    self.wfile.write(b"data: " + body + b"\n\n")
                except queue.Empty:
                    self.wfile.write(b": keepalive\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            return
        finally:
            with SSE_LOCK:
                SSE_CLIENTS.discard(q)

    # ---- routing ----
    def do_GET(self):
        path = unquote(urlparse(self.path).path)

        if path == "/" or path == "/index.html":
            self._send_static("index.html")
            return
        if path == "/favicon.ico":
            self._send_bytes(204, b"", CONTENT_TYPES[".ico"])
            return
        if path.startswith("/css/"):
            self._send_static("css/" + path[len("/css/"):])
            return
        if path.startswith("/js/"):
            self._send_static("js/" + path[len("/js/"):])
            return
        if path.startswith("/vendor/"):
            # #394: 同梱 Leaflet (web/vendor/leaflet/)。実 backend の /vendor/ route と対応。
            self._send_static("vendor/" + path[len("/vendor/"):])
            return

        if path == "/api/events":
            self._send_sse()
            return
        if path == "/api/maps":
            self._send_json({"maps": STATE["maps"], "current_map": STATE["current_map"]})
            return
        m = re.match(r"^/api/maps/([^/]+)/metadata$", path)
        if m:
            self._send_json(STATE["metadata"])
            return
        m = re.match(r"^/api/maps/([^/]+)/image$", path)
        if m:
            self._send_bytes(200, MAP_PNG, CONTENT_TYPES[".png"])
            return
        if path == "/api/pois":
            self._send_json({
                "pois": STATE["pois"],
                "map_name": STATE["current_map"],
                "config_version": STATE["config_version"],
            })
            return
        if path == "/api/routes":
            self._send_json({"routes": STATE["routes"], "map_name": STATE["current_map"]})
            return
        if path == "/api/tag-definitions":
            self._send_json({"tags": STATE["tags"]})
            return
        if path == "/api/nav/status":
            self._send_json(STATE["nav_status"])
            return
        if path == "/api/mode":
            self._send_json({"navigation": STATE["nav_status"]["navigation"]})
            return

        self._send_json({"error": "not found", "path": path}, 404)

    def do_HEAD(self):
        self.do_GET()

    def do_POST(self):
        path = unquote(urlparse(self.path).path)
        length = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(length) if length else b""
        if path == "/test/sse-event":
            # e2e 専用: body の JSON event を接続中の全 SSE client へ配信 (#384)。
            try:
                event = json.loads(body)
            except (ValueError, UnicodeDecodeError):
                self._send_json({"error": "invalid json"}, 400)
                return
            with SSE_LOCK:
                clients = list(SSE_CLIENTS)
            for q in clients:
                try:
                    q.put_nowait(event)
                except queue.Full:
                    pass
            self._send_json({"success": True, "clients": len(clients)})
            return
        if path == "/api/pois":
            self._send_json({"success": True, "config_version": STATE["config_version"]})
            return
        # routes / custom-tags / editor/select-map / nav/* など一律 success
        self._send_json({"success": True})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8799)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    print(f"[mock_server] serving {WEB_DIR} on http://{args.host}:{args.port}",
          file=sys.stderr, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
