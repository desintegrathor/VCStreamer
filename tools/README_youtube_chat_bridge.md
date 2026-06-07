# YouTube Chat Bridge

1. Build and run the receiver DLL once so `vcstreamer.ini` exists next to the DLL.
2. For no-Cloud-Console mode, install:

```powershell
python -m pip install --user chat-downloader
```

3. In `vcstreamer.ini`, set:

```ini
[YouTubeChat]
enabled=1
auth_mode=web
stream_url=https://youtube.com/live/xd1t2lS0Xb4?feature=share
video_id=
max_chars=480
```

Official YouTube API mode is still available by setting `auth_mode=official` and providing `oauth_client_secret`.

4. Start Vietcong with VCStreamer, connect to the server, and wait until the named pipe exists.
5. Run:

```powershell
python tools\youtube_chat_bridge.py --config path\to\vcstreamer.ini
```

The helper sends JSON author/message records to the receiver. The receiver submits them through VCHD VCAC streamer frames, and the server broadcasts red text.

Fast pipe/VCAC smoke test without YouTube:

```powershell
python tools\youtube_chat_bridge.py --config path\to\vcstreamer.ini --test-line "hello from pipe"
```
