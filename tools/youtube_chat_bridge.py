#!/usr/bin/env python3
import argparse
import ctypes
import http.server
import json
import os
import queue
import re
import socketserver
import sys
import threading
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from ctypes import wintypes


PIPE_NAME = r"\\.\pipe\vcstreamer_youtube_chat"
YOUTUBE_SCOPE = "https://www.googleapis.com/auth/youtube.readonly"
TOKEN_SKEW_SECONDS = 60


if os.name == "nt":
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _CreateFileW = _kernel32.CreateFileW
    _CreateFileW.argtypes = [
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.LPVOID,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    ]
    _CreateFileW.restype = wintypes.HANDLE

    _WriteFile = _kernel32.WriteFile
    _WriteFile.argtypes = [
        wintypes.HANDLE,
        wintypes.LPCVOID,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        wintypes.LPVOID,
    ]
    _WriteFile.restype = wintypes.BOOL

    _CloseHandle = _kernel32.CloseHandle
    _CloseHandle.argtypes = [wintypes.HANDLE]
    _CloseHandle.restype = wintypes.BOOL

    _WaitNamedPipeW = _kernel32.WaitNamedPipeW
    _WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
    _WaitNamedPipeW.restype = wintypes.BOOL

    _GENERIC_WRITE = 0x40000000
    _OPEN_EXISTING = 3
    _INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value
    _ERROR_PIPE_BUSY = 231
    _ERROR_FILE_NOT_FOUND = 2


class WindowsPipeWriter:
    def __init__(self, handle):
        self.handle = handle

    def write_bytes(self, data):
        written = wintypes.DWORD(0)
        buf = ctypes.create_string_buffer(data)
        ok = _WriteFile(self.handle, buf, len(data), ctypes.byref(written), None)
        if not ok or written.value != len(data):
            err = ctypes.get_last_error()
            raise OSError(err, ctypes.FormatError(err))

    def close(self):
        if self.handle and self.handle != _INVALID_HANDLE_VALUE:
            _CloseHandle(self.handle)
            self.handle = None


SPECIAL_TRANSLITERATION = {
    "\u00df": "ss", "\u1e9e": "SS",
    "\u00c6": "AE", "\u00e6": "ae",
    "\u0152": "OE", "\u0153": "oe",
    "\u00d8": "O", "\u00f8": "o",
    "\u0110": "D", "\u0111": "d",
    "\u00d0": "D", "\u00f0": "d",
    "\u00de": "Th", "\u00fe": "th",
    "\u0141": "L", "\u0142": "l",
    "\u014a": "N", "\u014b": "n",
}


def read_config(path):
    section = {}
    current_section = ""
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for raw_line in f:
                line = raw_line.strip()
                if not line or line.startswith(";") or line.startswith("#"):
                    continue
                if line.startswith("[") and line.endswith("]"):
                    current_section = line[1:-1].strip().lower()
                    continue
                if current_section != "youtubechat" or "=" not in line:
                    continue
                key, value = line.split("=", 1)
                section[key.strip().lower()] = value.strip()

    def get(name, default=""):
        return str(section.get(name, default)).strip()

    def get_int(name, default):
        try:
            return int(get(name, default))
        except ValueError:
            return default

    def get_bool(name, default=False):
        return get_int(name, 1 if default else 0) != 0

    base_dir = os.path.dirname(os.path.abspath(path))

    def resolve(value):
        if not value:
            return value
        return value if os.path.isabs(value) else os.path.join(base_dir, value)

    return {
        "enabled": get_bool("enabled", False),
        "auth_mode": get("auth_mode", "official").lower(),
        "stream_url": get("stream_url"),
        "live_chat_id": get("live_chat_id"),
        "video_id": get("video_id"),
        "oauth_client_secret": resolve(get("oauth_client_secret", r"tools\youtube_client_secret.json")),
        "oauth_token_cache": resolve(get("oauth_token_cache", r"tools\youtube_token.json")),
        "prefix": get("prefix", "[YT]"),
        "max_chars": max(1, min(480, get_int("max_chars", 480))),
        "drop_own_messages": get_bool("drop_own_messages", True),
        "fallback_polling": get_bool("fallback_polling", True),
        "send_initial_history": get_bool("send_initial_history", False),
    }


def sanitize_text(value):
    value = str(value).replace("\r", " ").replace("\n", " ").replace("\t", " ")
    for source, replacement in SPECIAL_TRANSLITERATION.items():
        value = value.replace(source, replacement)
    value = unicodedata.normalize("NFD", value)
    value = "".join(ch for ch in value if unicodedata.category(ch) != "Mn")
    value = "".join(ch for ch in value if 0x20 <= ord(ch) <= 0x7E)
    value = re.sub(r"\s+", " ", value).strip()
    return value


def make_chat_payload(author, message, max_chars):
    author = sanitize_text(author) or "viewer"
    message = sanitize_text(message)
    if not message:
        return None

    if author.startswith("@"):
        author = author[1:].strip()
    author = author[:64].strip() or "viewer"
    return {
        "author": author,
        "message": message[:max_chars].strip(),
    }


def http_json(url, token, timeout=60):
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code}: {body}") from exc


def http_json_public(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def post_json_public(url, data, timeout=60):
    req = urllib.request.Request(
        url,
        data=json.dumps(data).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "User-Agent": "Mozilla/5.0",
            "Origin": "https://www.youtube.com",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def post_form(url, data, timeout=30):
    encoded = urllib.parse.urlencode(data).encode("utf-8")
    req = urllib.request.Request(url, data=encoded, headers={"Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def load_client_secret(path):
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    data = raw.get("installed") or raw.get("web") or raw
    return {
        "client_id": data["client_id"],
        "client_secret": data.get("client_secret", ""),
        "auth_uri": data.get("auth_uri", "https://accounts.google.com/o/oauth2/v2/auth"),
        "token_uri": data.get("token_uri", "https://oauth2.googleapis.com/token"),
    }


class OAuthRedirectHandler(http.server.BaseHTTPRequestHandler):
    auth_code_queue = None

    def log_message(self, fmt, *args):
        return

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        code = params.get("code", [""])[0]
        error = params.get("error", [""])[0]
        if code:
            self.auth_code_queue.put(("code", code))
            body = b"OAuth complete. You can close this tab."
        else:
            self.auth_code_queue.put(("error", error or "Missing authorization code"))
            body = b"OAuth failed. Check the helper console."
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def save_token(path, token):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(token, f, indent=2, sort_keys=True)


def load_token(path):
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def token_valid(token):
    return token and token.get("access_token") and token.get("expires_at", 0) > time.time() + TOKEN_SKEW_SECONDS


def refresh_token(client, token, token_path):
    if not token or not token.get("refresh_token"):
        return None
    refreshed = post_form(client["token_uri"], {
        "client_id": client["client_id"],
        "client_secret": client["client_secret"],
        "refresh_token": token["refresh_token"],
        "grant_type": "refresh_token",
    })
    token.update(refreshed)
    token["expires_at"] = time.time() + int(refreshed.get("expires_in", 3600))
    save_token(token_path, token)
    return token


def oauth_interactive(client, token_path):
    code_queue = queue.Queue()
    OAuthRedirectHandler.auth_code_queue = code_queue
    with socketserver.TCPServer(("127.0.0.1", 0), OAuthRedirectHandler) as server:
        port = server.server_address[1]
        redirect_uri = f"http://127.0.0.1:{port}/oauth2callback"
        thread = threading.Thread(target=server.handle_request, daemon=True)
        thread.start()

        params = {
            "client_id": client["client_id"],
            "redirect_uri": redirect_uri,
            "response_type": "code",
            "scope": YOUTUBE_SCOPE,
            "access_type": "offline",
            "prompt": "consent",
        }
        url = client["auth_uri"] + "?" + urllib.parse.urlencode(params)
        print(f"Opening OAuth browser: {url}")
        webbrowser.open(url)

        kind, value = code_queue.get(timeout=300)
        if kind != "code":
            raise RuntimeError(value)

        token = post_form(client["token_uri"], {
            "client_id": client["client_id"],
            "client_secret": client["client_secret"],
            "code": value,
            "grant_type": "authorization_code",
            "redirect_uri": redirect_uri,
        })
        token["expires_at"] = time.time() + int(token.get("expires_in", 3600))
        save_token(token_path, token)
        return token


def get_access_token(config):
    client = load_client_secret(config["oauth_client_secret"])
    token = load_token(config["oauth_token_cache"])
    if token_valid(token):
        return token["access_token"]
    token = refresh_token(client, token, config["oauth_token_cache"])
    if token_valid(token):
        return token["access_token"]
    token = oauth_interactive(client, config["oauth_token_cache"])
    return token["access_token"]


def api_url(path, params):
    return "https://www.googleapis.com/youtube/v3/" + path + "?" + urllib.parse.urlencode(params)


def resolve_live_chat_id(config, token):
    if config["live_chat_id"]:
        return config["live_chat_id"]

    if config["video_id"]:
        data = http_json(api_url("videos", {
            "part": "liveStreamingDetails",
            "id": config["video_id"],
        }), token)
        for item in data.get("items", []):
            live = item.get("liveStreamingDetails", {})
            chat_id = live.get("activeLiveChatId")
            if chat_id:
                return chat_id

    data = http_json(api_url("liveBroadcasts", {
        "part": "snippet",
        "broadcastStatus": "active",
        "broadcastType": "all",
        "mine": "true",
    }), token)
    for item in data.get("items", []):
        chat_id = item.get("snippet", {}).get("liveChatId")
        if chat_id:
            return chat_id

    raise RuntimeError("No liveChatId found. Set YouTubeChat.live_chat_id or video_id in vcstreamer.ini.")


def resolve_stream_url(config):
    if config["stream_url"]:
        return config["stream_url"]
    if config["video_id"]:
        return "https://www.youtube.com/watch?v=" + config["video_id"]
    raise RuntimeError("No stream URL found. Set YouTubeChat.stream_url or video_id in vcstreamer.ini.")


def extract_json_assignment(html, name):
    match = re.search(re.escape(name) + r"\s*=\s*", html)
    if not match:
        return None

    index = match.end()
    while index < len(html) and html[index].isspace():
        index += 1
    if index >= len(html) or html[index] not in "[{":
        return None

    start = index
    depth = 0
    in_string = False
    escaped = False
    for pos in range(index, len(html)):
        ch = html[pos]
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
        else:
            if ch == '"':
                in_string = True
            elif ch in "[{":
                depth += 1
            elif ch in "]}":
                depth -= 1
                if depth == 0:
                    return json.loads(html[start:pos + 1])
    return None


def find_first_key(value, key):
    if isinstance(value, dict):
        if key in value:
            return value[key]
        for child in value.values():
            found = find_first_key(child, key)
            if found is not None:
                return found
    elif isinstance(value, list):
        for child in value:
            found = find_first_key(child, key)
            if found is not None:
                return found
    return None


def text_runs_to_plain(text_obj):
    if not isinstance(text_obj, dict):
        return ""
    if "simpleText" in text_obj:
        return str(text_obj.get("simpleText", ""))
    parts = []
    for run in text_obj.get("runs", []):
        if "text" in run:
            parts.append(str(run.get("text", "")))
        elif "emoji" in run:
            parts.append(str(run.get("emoji", {}).get("shortcuts", [""])[0]))
    return "".join(parts)


def fetch_youtube_watch_page(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return resp.read().decode("utf-8", errors="replace")


def parse_innertube_bootstrap(html):
    api_key_match = re.search(r'"INNERTUBE_API_KEY"\s*:\s*"([^"]+)"', html)
    client_version_match = re.search(r'"INNERTUBE_CLIENT_VERSION"\s*:\s*"([^"]+)"', html)
    if not api_key_match or not client_version_match:
        raise RuntimeError("Unable to find YouTube Innertube bootstrap values.")

    initial_data = extract_json_assignment(html, "ytInitialData")
    if not initial_data:
        raise RuntimeError("Unable to parse ytInitialData.")

    live_chat = find_first_key(initial_data, "liveChatRenderer")
    if not live_chat:
        raise RuntimeError("Unable to find live chat renderer. Is chat enabled for this stream?")

    continuation = None
    for item in live_chat.get("continuations", []):
        continuation = (
            item.get("reloadContinuationData", {}).get("continuation")
            or item.get("timedContinuationData", {}).get("continuation")
            or item.get("invalidationContinuationData", {}).get("continuation")
        )
        if continuation:
            break
    if not continuation:
        raise RuntimeError("Unable to find initial live chat continuation.")

    return api_key_match.group(1), client_version_match.group(1), continuation


def next_continuation_and_timeout(live_chat_continuation):
    for item in live_chat_continuation.get("continuations", []):
        data = (
            item.get("timedContinuationData")
            or item.get("invalidationContinuationData")
            or item.get("reloadContinuationData")
        )
        if data and data.get("continuation"):
            timeout_ms = int(data.get("timeoutMs", data.get("timeoutMs", 5000)) or 5000)
            return data["continuation"], max(1000, timeout_ms)
    return "", 5000


def run_innertube(config, pipe):
    url = resolve_stream_url(config)
    print(f"Using unauthenticated Innertube mode: {url}")
    html = fetch_youtube_watch_page(url)
    api_key, client_version, continuation = parse_innertube_bootstrap(html)
    endpoint = "https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?key=" + api_key
    seen_ids = set()
    emit = config["send_initial_history"]

    while continuation:
        body = {
            "context": {
                "client": {
                    "clientName": "WEB",
                    "clientVersion": client_version,
                    "hl": "en",
                    "gl": "US",
                }
            },
            "continuation": continuation,
        }
        data = post_json_public(endpoint, body, timeout=60)
        live_chat = data.get("continuationContents", {}).get("liveChatContinuation", {})
        for action in live_chat.get("actions", []):
            item = action.get("addChatItemAction", {}).get("item", {})
            renderer = item.get("liveChatTextMessageRenderer")
            if not renderer:
                continue

            msg_id = str(renderer.get("id", ""))
            if msg_id and msg_id in seen_ids:
                continue
            if msg_id:
                seen_ids.add(msg_id)

            item = make_chat_payload(
                text_runs_to_plain(renderer.get("authorName", {})),
                text_runs_to_plain(renderer.get("message", {})),
                config["max_chars"],
            )
            if item and emit:
                print(f"{item['author']}: {item['message']}")
                send_pipe_line(pipe, item)

        emit = True
        continuation, timeout_ms = next_continuation_and_timeout(live_chat)
        time.sleep(timeout_ms / 1000.0)


def connect_pipe():
    while True:
        if os.name == "nt":
            handle = _CreateFileW(PIPE_NAME, _GENERIC_WRITE, 0, None, _OPEN_EXISTING, 0, None)
            if handle != _INVALID_HANDLE_VALUE:
                return WindowsPipeWriter(handle)

            err = ctypes.get_last_error()
            if err == _ERROR_PIPE_BUSY:
                _WaitNamedPipeW(PIPE_NAME, 2000)
                continue
            print(f"Waiting for receiver pipe {PIPE_NAME}: {ctypes.FormatError(err).strip()}")
            time.sleep(2)
        else:
            try:
                return open(PIPE_NAME, "w", encoding="utf-8", newline="\n")
            except OSError as exc:
                print(f"Waiting for receiver pipe {PIPE_NAME}: {exc}")
                time.sleep(2)


def send_pipe_line(pipe, item):
    line = json.dumps({
        "author": item.get("author", "viewer"),
        "message": item.get("message", ""),
    }, ensure_ascii=True) + "\n"
    data = line.encode("utf-8")
    try:
        if hasattr(pipe, "write_bytes"):
            pipe.write_bytes(data)
        else:
            pipe.write(line)
            pipe.flush()
    except OSError:
        try:
            pipe.close()
        except Exception:
            pass
        if hasattr(pipe, "write_bytes"):
            replacement = connect_pipe()
            pipe.handle = replacement.handle
            replacement.handle = None
            pipe.write_bytes(data)
        else:
            raise


def extract_text_message(item, config):
    snippet = item.get("snippet", {})
    author = item.get("authorDetails", {})

    if snippet.get("type") != "textMessageEvent":
        return ""
    if config["drop_own_messages"] and author.get("isChatOwner"):
        return ""

    message = snippet.get("textMessageDetails", {}).get("messageText", "")
    return make_chat_payload(author.get("displayName", "viewer"), message, config["max_chars"])


def process_response(data, seen_ids, pipe, config, emit):
    for item in data.get("items", []):
        msg_id = item.get("id", "")
        if msg_id and msg_id in seen_ids:
            continue
        if msg_id:
            seen_ids.add(msg_id)
        chat_item = extract_text_message(item, config)
        if chat_item and emit:
            print(f"{chat_item['author']}: {chat_item['message']}")
            send_pipe_line(pipe, chat_item)


def run_stream(config, token, chat_id, pipe):
    seen_ids = set()
    page_token = ""
    emit = config["send_initial_history"]

    while True:
        params = {
            "part": "snippet,authorDetails",
            "liveChatId": chat_id,
            "maxResults": "200",
        }
        if page_token:
            params["pageToken"] = page_token
        data = http_json(api_url("liveChat/messages/streamList", params), token, timeout=300)
        process_response(data, seen_ids, pipe, config, emit)
        emit = True
        page_token = data.get("nextPageToken", page_token)
        if data.get("offlineAt"):
            print("Live chat is offline.")
            return


def run_polling(config, token, chat_id, pipe):
    seen_ids = set()
    page_token = ""
    emit = config["send_initial_history"]

    while True:
        params = {
            "part": "snippet,authorDetails",
            "liveChatId": chat_id,
            "maxResults": "200",
        }
        if page_token:
            params["pageToken"] = page_token
        data = http_json(api_url("liveChat/messages", params), token, timeout=60)
        process_response(data, seen_ids, pipe, config, emit)
        emit = True
        page_token = data.get("nextPageToken", page_token)
        if data.get("offlineAt"):
            print("Live chat is offline.")
            return
        interval = max(1000, int(data.get("pollingIntervalMillis", 5000))) / 1000.0
        time.sleep(interval)


def run_chat_downloader(config, pipe):
    try:
        from chat_downloader import ChatDownloader
    except ImportError as exc:
        raise RuntimeError(
            "chat-downloader is not installed. Run: python -m pip install --user chat-downloader"
        ) from exc

    url = resolve_stream_url(config)
    print(f"Using unauthenticated chat-downloader mode: {url}")
    seen_ids = set()
    chat = ChatDownloader().get_chat(url, chat_type="live")

    for item in chat:
        msg_id = str(item.get("message_id", ""))
        if msg_id and msg_id in seen_ids:
            continue
        if msg_id:
            seen_ids.add(msg_id)

        if item.get("message_type") not in ("text_message", None):
            continue

        author = item.get("author") or {}
        chat_item = make_chat_payload(
            author.get("name", "viewer"),
            item.get("message", ""),
            config["max_chars"],
        )
        if chat_item:
            print(f"{chat_item['author']}: {chat_item['message']}")
            send_pipe_line(pipe, chat_item)


def main():
    parser = argparse.ArgumentParser(description="Mirror YouTube live chat into VCStreamer receiver chat pipe.")
    parser.add_argument("--config", default="vcstreamer.ini", help="Path to vcstreamer.ini next to the receiver DLL.")
    parser.add_argument("--poll", action="store_true", help="Use polling immediately instead of streamList.")
    parser.add_argument("--test-line", help="Send one sanitized test line to the receiver pipe and exit.")
    args = parser.parse_args()

    config = read_config(args.config)
    if args.test_line:
        pipe = connect_pipe()
        text = sanitize_text(args.test_line)[:config["max_chars"]].strip()
        if text:
            item = {"author": "test", "message": text}
            send_pipe_line(pipe, item)
            print(f"{item['author']}: {item['message']}")
        return 0

    if not config["enabled"]:
        print("YouTubeChat.enabled=0 in config; exiting.")
        return 0

    if config["auth_mode"] in ("web", "chat_downloader", "chat-downloader", "unauthenticated"):
        pipe = connect_pipe()
        try:
            run_innertube(config, pipe)
        except Exception as exc:
            print(f"Innertube mode failed, falling back to chat-downloader: {exc}", file=sys.stderr)
            run_chat_downloader(config, pipe)
        return 0

    if not os.path.exists(config["oauth_client_secret"]):
        print("OAuth client secret not found; falling back to unauthenticated chat-downloader mode.")
        pipe = connect_pipe()
        run_chat_downloader(config, pipe)
        return 0

    token = get_access_token(config)
    chat_id = resolve_live_chat_id(config, token)
    print(f"Using liveChatId={chat_id}")

    pipe = connect_pipe()
    backoff = 1
    while True:
        try:
            if args.poll:
                run_polling(config, token, chat_id, pipe)
            else:
                run_stream(config, token, chat_id, pipe)
            return 0
        except KeyboardInterrupt:
            return 0
        except Exception as exc:
            print(f"YouTube chat bridge error: {exc}", file=sys.stderr)
            if not config["fallback_polling"] and not args.poll:
                raise
            try:
                pipe.close()
            except Exception:
                pass
            pipe = connect_pipe()
            token = get_access_token(config)
            args.poll = True
            time.sleep(backoff)
            backoff = min(backoff * 2, 30)


if __name__ == "__main__":
    raise SystemExit(main())
