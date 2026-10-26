"""Serves the PicoGB loader page on http://localhost:8642 and opens it.

The page talks to the Sprig over Web Serial, which only works on secure
origins; localhost counts as one, hence this small server. Use Chrome or Edge.

    python serve.py              # serve and open the browser
    python serve.py --no-browser
"""
import functools
import http.server
import os
import sys
import threading
import webbrowser

PORT = 8642


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=here)
    handler.log_message = lambda *a: None
    try:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), handler)
    except OSError:
        server = None  # already running: just open the page
    url = f"http://localhost:{PORT}/"
    if "--no-browser" not in sys.argv:
        threading.Timer(0.3, webbrowser.open, (url,)).start()
    if server:
        print(f"PicoGB loader at {url} (close this window to stop it)")
        server.serve_forever()


if __name__ == "__main__":
    main()
