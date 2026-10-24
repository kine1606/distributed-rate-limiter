#!/usr/bin/env python3
"""Live view of a running limiter: serves dashboard/live.html and relays the limiter's Prometheus text
at /metrics from the same origin, so the limiter's metrics endpoint needs no CORS header. /phase
returns the demo's current phase (scripts/live_demo.sh writes it to a file).

    tools/live_dashboard.py [--listen 127.0.0.1:8090] [--metrics http://127.0.0.1:9100/metrics] [--phase-file F]
"""
import argparse
import http.server
import pathlib
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="127.0.0.1:8090")  # loopback only: the page shows the limiter's state
    ap.add_argument("--metrics", default="http://127.0.0.1:9100/metrics")
    ap.add_argument("--phase-file")
    a = ap.parse_args()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/":
                body, ctype = (ROOT / "dashboard/live.html").read_bytes(), "text/html; charset=utf-8"
            elif self.path == "/metrics":
                try:
                    with urllib.request.urlopen(a.metrics, timeout=2) as r:
                        body = r.read()
                except OSError as e:
                    self.send_error(502, f"limiter metrics unreachable at {a.metrics}: {e}")
                    return
                ctype = "text/plain; version=0.0.4"
            elif self.path == "/phase":
                f = pathlib.Path(a.phase_file) if a.phase_file else None
                body, ctype = (f.read_bytes() if f and f.exists() else b""), "text/plain; charset=utf-8"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):  # one line per second per tab is noise
            pass

    host, port = a.listen.rsplit(":", 1)
    print(f"live view: http://{a.listen}/  (metrics from {a.metrics})", flush=True)
    http.server.ThreadingHTTPServer((host, int(port)), Handler).serve_forever()


if __name__ == "__main__":
    main()
