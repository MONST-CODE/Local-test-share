#!/usr/bin/env python3
"""SAME_HOST_INTEGRATION: run the real DevLink demo HTTP service and verify host state.

This proves the demo service's actual HTTP routes and mutations only. It does not
exercise WebView2, a guest renderer, TLS media, or a physical LAN.
"""
from __future__ import annotations

import http.client
import json
import select
import subprocess
import sys
import time
from urllib.parse import urlencode


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: demo_service_test.py <devlink-demo-service>", file=sys.stderr)
        return 2

    process = subprocess.Popen(
        [sys.argv[1], "--host", "127.0.0.1", "--port", "0", "--quiet"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    connection: http.client.HTTPConnection | None = None
    try:
        assert process.stdout is not None
        ready, _, _ = select.select([process.stdout], [], [], 5.0)
        require(bool(ready), "demo service did not announce an ephemeral port within five seconds")
        line = process.stdout.readline().strip()
        require(line.startswith("port="), f"unexpected service startup output: {line!r}")
        port = int(line.partition("=")[2])
        require(0 < port <= 65535, f"invalid bound port: {port}")

        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=4.0)

        def request(method: str, path: str, body: str | None = None, content_type: str | None = None):
            headers = {"Connection": "close"}
            if content_type:
                headers["Content-Type"] = content_type
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            data = response.read()
            return response.status, response.getheader("Content-Type", ""), response.getheader("Content-Security-Policy", ""), data

        status, content_type, csp, page = request("GET", "/")
        require(status == 200 and "text/html" in content_type, "real index route did not return HTML")
        require(b"Application web locale" in page and b"id=\"increment\"" in page and b"id=\"feedback\"" in page,
                "index page lacks the recognizable app, real action, or form")
        require(b"/assets/demo-mark.svg" in page and b"/assets/app.js" in page and b"/details" in page,
                "index page lacks same-origin image, script, or second route")
        require("default-src 'self'" in csp and "object-src 'none'" in csp,
                "demo page is missing its restrictive same-origin CSP")

        status, content_type, _, details = request("GET", "/details")
        require(status == 200 and "text/html" in content_type and b"Page d\xc3\xa9tails" in details,
                "second real page route is missing")
        status, content_type, _, image = request("GET", "/assets/demo-mark.svg")
        require(status == 200 and "image/svg+xml" in content_type and b"<svg" in image,
                "same-origin image resource did not load")
        status, content_type, _, script = request("GET", "/assets/app.js")
        require(status == 200 and "javascript" in content_type and b"/api/action" in script,
                "interactive browser script did not load")

        status, _, _, initial_bytes = request("GET", "/api/status")
        initial = json.loads(initial_bytes)
        require(status == 200 and initial["actions"] == 0, "initial server action state is not clean")

        status, _, _, increment_bytes = request(
            "POST", "/api/action", urlencode({"action": "increment"}), "application/x-www-form-urlencoded"
        )
        incremented = json.loads(increment_bytes)
        require(status == 200 and incremented["actions"] == 1, "button action did not mutate host service state")

        status, _, _, submit_bytes = request(
            "POST", "/api/action", urlencode({"action": "submit", "value": "Guest 42"}),
            "application/x-www-form-urlencoded",
        )
        submitted = json.loads(submit_bytes)
        require(status == 200 and submitted["actions"] == 2 and submitted["last_value"] == "Guest 42",
                "submitted form was not validated and retained by the host service")

        status, _, _, after_bytes = request("GET", "/api/status")
        after = json.loads(after_bytes)
        require(status == 200 and after["actions"] == 2 and after["last_value"] == "Guest 42",
                "host-observable state did not persist between real HTTP requests")

        status, _, _, invalid = request(
            "POST", "/api/action", urlencode({"action": "submit", "value": 'x" onmouseover="bad'}),
            "application/x-www-form-urlencoded",
        )
        require(status == 400 and json.loads(invalid).get("error"), "unsafe form text was not rejected")
        status, _, _, _ = request("GET", "/not-allowed")
        require(status == 404, "unknown route was not rejected")
        status, _, _, _ = request("GET", "/api/action")
        require(status == 405, "mutating route accepted the wrong HTTP method")

        print("SAME_HOST_INTEGRATION PASS: real service, routes, SVG, JS, CSP, POST actions and persistent host state")
        return 0
    finally:
        if connection is not None:
            connection.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2.0)
        if process.returncode not in (0, None):
            err = process.stderr.read() if process.stderr is not None else ""
            raise RuntimeError(f"demo service exited {process.returncode}: {err}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # test runner prints a useful non-zero diagnostic.
        print(f"SAME_HOST_INTEGRATION FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
