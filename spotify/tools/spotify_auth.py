#!/usr/bin/env python3
"""One-time Spotify OAuth on the host computer to obtain a refresh token
for tools/spotify_bridge.py.

In the Spotify app dashboard register redirect URI:  http://127.0.0.1:8888/callback
Run:  CLIENT_ID=xxx CLIENT_SECRET=yyy python3 spotify_auth.py
It opens the browser, you click Agree, and it prints the REFRESH TOKEN.
"""
import base64, http.server, os, sys, urllib.parse, urllib.request, webbrowser

CLIENT_ID = os.environ.get("CLIENT_ID") or (sys.argv[1] if len(sys.argv) > 1 else "")
CLIENT_SECRET = os.environ.get("CLIENT_SECRET") or (sys.argv[2] if len(sys.argv) > 2 else "")
REDIRECT = "http://127.0.0.1:8888/callback"
SCOPE = "user-read-currently-playing user-read-playback-state user-modify-playback-state"

if not CLIENT_ID or not CLIENT_SECRET:
    sys.exit("Set CLIENT_ID and CLIENT_SECRET (env vars or args).")

auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
    "client_id": CLIENT_ID, "response_type": "code",
    "redirect_uri": REDIRECT, "scope": SCOPE,
})

result = {}

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        q = urllib.parse.urlparse(self.path)
        if q.path != "/callback":
            self.send_response(404); self.end_headers(); return
        params = urllib.parse.parse_qs(q.query)
        code = params.get("code", [None])[0]
        result["code"] = code
        self.send_response(200)
        self.send_header("Content-Type", "text/html"); self.end_headers()
        self.wfile.write(b"<h2 style='font-family:sans-serif'>OK - you can return to the terminal.</h2>")

def exchange(code):
    basic = base64.b64encode(f"{CLIENT_ID}:{CLIENT_SECRET}".encode()).decode()
    data = urllib.parse.urlencode({
        "grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT,
    }).encode()
    req = urllib.request.Request("https://accounts.spotify.com/api/token", data=data,
                                 headers={"Authorization": "Basic " + basic,
                                          "Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req) as r:
        import json
        return json.load(r)

print("Opening the browser for Spotify authorization...")
webbrowser.open(auth_url)
print("If it does not open, go to:\n" + auth_url + "\n")

srv = http.server.HTTPServer(("127.0.0.1", 8888), H)
while "code" not in result:
    srv.handle_request()

if not result["code"]:
    sys.exit("No code received (authorization cancelled?).")

tok = exchange(result["code"])
rt = tok.get("refresh_token")
if not rt:
    sys.exit("Error: " + str(tok))

print("\n================ REFRESH TOKEN ================\n" + rt +
      "\n==============================================")
