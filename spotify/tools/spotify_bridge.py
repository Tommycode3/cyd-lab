#!/usr/bin/env python3
"""CYD Spotify bridge — runs on the host computer, links Spotify <-> CYD over USB serial.

The host talks to Spotify; the CYD just renders + sends button taps.
  host -> CYD:  T<US>title<US>artist<US>album<US>dur<US>prog<US>playing   (track)
               P<US>prog<US>playing                                       (progress)
               J<US>len\\n<len bytes JPEG>                                 (album art)
  CYD -> host:  N next | B back | S toggle | #... debug
Usage:  python3 spotify_bridge.py [/dev/cu.usbserial-XXXX]
Credentials: env vars or tools/spotify_credentials.json (see load_credentials).
"""
import base64, glob, http.client, io, json, os, sys, time, urllib.parse
import serial
try:
    from PIL import Image
except Exception:
    Image = None

COVER = 148   # send art pre-sized to the CYD cover box (tiny -> instant over serial)


def resize_jpeg(raw):
    if not Image:
        return raw
    try:
        im = Image.open(io.BytesIO(raw)).convert("RGB").resize((COVER, COVER), Image.LANCZOS)
        out = io.BytesIO(); im.save(out, "JPEG", quality=85)
        return out.getvalue()
    except Exception:
        return raw

def load_credentials():
    """Read Spotify credentials from env vars, else from a gitignored JSON file.

    Env vars:  SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, SPOTIFY_REFRESH_TOKEN
    File:      spotify_credentials.json next to this script (see the .example file),
               or the path in SPOTIFY_CREDENTIALS_FILE.
    """
    keys = ("SPOTIFY_CLIENT_ID", "SPOTIFY_CLIENT_SECRET", "SPOTIFY_REFRESH_TOKEN")
    creds = {k: os.environ.get(k, "") for k in keys}
    if not all(creds.values()):
        path = os.environ.get("SPOTIFY_CREDENTIALS_FILE") or os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "spotify_credentials.json")
        if os.path.exists(path):
            with open(path) as f:
                data = json.load(f)
            for k in keys:
                creds[k] = creds[k] or data.get(k, "")
    missing = [k for k in keys if not creds[k]]
    if missing:
        sys.exit("Missing Spotify credentials: " + ", ".join(missing) +
                 "\nSet them as env vars or in tools/spotify_credentials.json.")
    return creds[keys[0]], creds[keys[1]], creds[keys[2]]


CLIENT_ID, CLIENT_SECRET, REFRESH_TOKEN = load_credentials()

PORT = sys.argv[1] if len(sys.argv) > 1 else ""   # empty = auto-detect
BAUD = 460800
US = "\x1f"


def clean(s):
    return (s or "").replace("\x1f", " ").replace("\n", " ").replace("\r", " ")


class Spotify:
    def __init__(self):
        self.token = None
        self.exp = 0
        self.api = None
        self.img = None
        self.img_host = None

    def _refresh(self):
        c = http.client.HTTPSConnection("accounts.spotify.com", timeout=8)
        body = urllib.parse.urlencode({"grant_type": "refresh_token", "refresh_token": REFRESH_TOKEN})
        auth = base64.b64encode(f"{CLIENT_ID}:{CLIENT_SECRET}".encode()).decode()
        c.request("POST", "/api/token", body,
                  {"Authorization": "Basic " + auth, "Content-Type": "application/x-www-form-urlencoded"})
        d = json.load(c.getresponse()); c.close()
        self.token = d["access_token"]; self.exp = time.time() + d.get("expires_in", 3600) - 60

    def _tok(self):
        if not self.token or time.time() > self.exp:
            self._refresh()
        return self.token

    def _apiconn(self):
        if self.api is None:
            self.api = http.client.HTTPSConnection("api.spotify.com", timeout=8)
        return self.api

    def req(self, method, path):
        for attempt in range(2):
            try:
                c = self._apiconn()
                c.request(method, path, headers={"Authorization": "Bearer " + self._tok()})
                r = c.getresponse()
                data = r.read()
                return r.status, data
            except Exception:
                try: self.api.close()
                except Exception: pass
                self.api = None
                time.sleep(0.05)
        return -1, b""

    def now(self):
        st, data = self.req("GET", "/v1/me/player/currently-playing?additional_types=track")
        if st == 200 and data:
            try: return json.loads(data)
            except Exception: return None
        return None

    def cmd(self, kind):
        if kind == "N": self.req("POST", "/v1/me/player/next")
        elif kind == "B": self.req("POST", "/v1/me/player/previous")
        elif kind == "S":
            # toggle based on last known state handled by caller
            pass

    def play(self):  self.req("PUT", "/v1/me/player/play")
    def pause(self): self.req("PUT", "/v1/me/player/pause")

    def image(self, url):
        u = urllib.parse.urlparse(url)
        if self.img is None or self.img_host != u.netloc:
            if self.img:
                try: self.img.close()
                except Exception: pass
            self.img = http.client.HTTPSConnection(u.netloc, timeout=8); self.img_host = u.netloc
        for attempt in range(2):
            try:
                self.img.request("GET", u.path)
                r = self.img.getresponse(); return r.read()
            except Exception:
                try: self.img.close()
                except Exception: pass
                self.img = http.client.HTTPSConnection(u.netloc, timeout=8)
        return None


def find_port():
    if PORT and glob.glob(PORT):
        return PORT
    cands = sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/cu.SLAB_USBtoUART*"))
    return cands[0] if cands else None


def open_serial():
    while True:
        p = find_port()
        if p:
            try:
                s = serial.Serial(p, BAUD, timeout=0)
                print(f"[bridge] connected {p} @ {BAUD}")
                time.sleep(1.6); s.reset_input_buffer()
                return s
            except Exception as e:
                print(f"[bridge] open failed: {e}")
        else:
            print("[bridge] waiting for CYD (plug in the cable)...")
        time.sleep(2)


def run(sp, ser):
    cur_key = None
    cur_img_url = None
    playing = False
    last_poll = 0
    last_prog_send = 0
    poll_now = True
    fast_until = 0.0        # after a command: poll rapidly until the change lands
    rx = b""

    def send_track(it, prog, play):
        line = ("T" + US + clean(it["name"]) + US +
                clean(", ".join(a["name"] for a in it["artists"])) + US +
                clean(it["album"]["name"]) + US + str(it["duration_ms"]) + US +
                str(prog) + US + ("1" if play else "0") + "\n")
        ser.write(line.encode("utf-8", "replace"))

    def w(data):
        ser.write(data)      # raises on unplug -> caught below to reconnect

    while True:
        # --- read button taps / debug from CYD ---
        try:
            b = ser.read(256)
            if b: rx += b
        except (serial.SerialException, OSError):
            print("[bridge] CYD disconnected"); return
        while b"\n" in rx:
            ln, rx = rx.split(b"\n", 1)
            s = ln.decode("utf-8", "replace").strip()
            if not s: continue
            if s.startswith("#"):
                print("  " + s)
                if s.startswith("#ready"):        # CYD (re)booted -> resend everything
                    cur_key = None; cur_img_url = None; poll_now = True
                continue
            c = s[0]
            if c in ("N", "B"):
                sp.cmd(c); poll_now = True; fast_until = time.time() + 1.5
            elif c == "S":
                (sp.pause() if playing else sp.play()); poll_now = True; fast_until = time.time() + 0.8

        # --- poll Spotify ---
        now = time.time()
        interval = 0.15 if now < fast_until else 0.5
        if poll_now or now - last_poll > interval:
            poll_now = False; last_poll = now
            data = sp.now()
            try:
                if data and data.get("item"):
                    it = data["item"]; prog = data.get("progress_ms", 0); playing = data.get("is_playing", False)
                    key = it["id"]
                    if key != cur_key:
                        cur_key = key
                        w(("T" + US + clean(it["name"]) + US +
                           clean(", ".join(a["name"] for a in it["artists"])) + US +
                           clean(it["album"]["name"]) + US + str(it["duration_ms"]) + US +
                           str(prog) + US + ("1" if playing else "0") + "\n").encode("utf-8", "replace"))
                        imgs = it["album"].get("images", [])
                        url = (imgs[1]["url"] if len(imgs) > 1 else imgs[0]["url"]) if imgs else None
                        if url and url != cur_img_url:
                            cur_img_url = url
                            raw = sp.image(url)
                            if raw:
                                raw = resize_jpeg(raw)
                                w(("J" + US + str(len(raw)) + "\n").encode()); w(raw)
                                print(f"[bridge] {clean(it['name'])} — art {len(raw)}B")
                    else:
                        w(("P" + US + str(prog) + US + ("1" if playing else "0") + "\n").encode())
            except (serial.SerialException, OSError):
                print("[bridge] CYD disconnected"); return
        time.sleep(0.02)


def main():
    sp = Spotify()
    while True:
        ser = open_serial()
        try:
            run(sp, ser)
        except Exception as e:
            print(f"[bridge] error: {e}")
        try: ser.close()
        except Exception: pass
        time.sleep(1)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[bridge] stop")
