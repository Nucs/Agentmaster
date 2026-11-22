"""
The "Run on the PC" button's receiver for the Teams proof: a bot-less card button is an Action.OpenUrl to a URL
that Agentmaster itself serves. This listener stands in for Agentmaster: it binds 127.0.0.1 only, answers the
first request to /a with a short page, logs what arrived (time, path, query, user agent) and exits. It also exits
after its time limit, so it never stays behind.

Usage: python teams_proof_listener.py <port> <log file> <seconds>
"""
import http.server
import sys
import threading
import time
import urllib.parse

PORT = int(sys.argv[1])
LOG = sys.argv[2]
LIMIT = int(sys.argv[3])
done = threading.Event()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802 (the http.server name)
        u = urllib.parse.urlparse(self.path)
        line = (f"{time.strftime('%Y-%m-%d %H:%M:%S')} GET {u.path} query={urllib.parse.parse_qs(u.query)} "
                f"ua={self.headers.get('User-Agent', '')[:160]}\n")
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line)
        if u.path != "/a":
            self.send_response(404)
            self.end_headers()
            return
        page = ("<!doctype html><meta charset=utf-8><title>Agentmaster</title>"
                "<body style='font:16px system-ui;background:#1e1e1e;color:#eee;padding:2em'>"
                "<h2>Agentmaster received the button</h2><p>teams-proof: the PC got <b>"
                + ",".join(urllib.parse.parse_qs(u.query).get("choice", ["?"])) +
                "</b>. You can close this tab.</p></body>").encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(page)))
        self.end_headers()
        self.wfile.write(page)
        done.set()

    def log_message(self, *args):  # keep stderr quiet; the log file has the facts
        pass


srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
threading.Thread(target=srv.serve_forever, daemon=True).start()
with open(LOG, "a", encoding="utf-8") as f:
    f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} listening on 127.0.0.1:{PORT} for {LIMIT} s\n")
clicked = done.wait(LIMIT)
time.sleep(1)  # let the page finish sending
srv.shutdown()
with open(LOG, "a", encoding="utf-8") as f:
    f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} exit: {'button received' if clicked else 'time limit, no button'}\n")
print("button received" if clicked else "time limit, no button")
