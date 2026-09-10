#!/usr/bin/env python3

import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse


fixture_path = Path(sys.argv[1])
port_path = Path(sys.argv[2])
request_log_path = Path(sys.argv[3]) if len(sys.argv) >= 4 else None
user_agent_log_path = Path(sys.argv[4]) if len(sys.argv) >= 5 else None
fixture = json.loads(fixture_path.read_text(encoding="utf-8"))
info_sequence_group_counts = {}
info_sequence_group_lock = threading.Lock()
request_log_lock = threading.Lock()


def sequenced_info_results(requested_name, packages):
    for group_name, group in fixture.get("info_sequence_groups", {}).items():
        if requested_name not in group.get("requests", []):
            continue

        with info_sequence_group_lock:
            request_index = info_sequence_group_counts.get(group_name, 0)
            info_sequence_group_counts[group_name] = request_index + 1

        result_overrides = group.get("result_overrides", [])
        if request_index >= len(result_overrides):
            return None
        result_override = result_overrides[request_index]
        if result_override is None:
            return None

        package = packages.get(requested_name)
        if not isinstance(package, dict) or not isinstance(result_override, dict):
            return None
        result = package.copy()
        result.update(result_override)
        return [result]
    return None


def apply_response_override(response, override):
    if not isinstance(override, dict):
        return response

    for field in override.get("remove", []):
        response.pop(field, None)
    response.update(override.get("set", {}))
    return response


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if request_log_path is not None or user_agent_log_path is not None:
            with request_log_lock:
                if request_log_path is not None:
                    with request_log_path.open("a", encoding="utf-8") as request_log:
                        request_log.write(f"{self.path}\n")
                if user_agent_log_path is not None:
                    with user_agent_log_path.open("a", encoding="utf-8") as user_agent_log:
                        user_agent_log.write(f"{self.headers.get('User-Agent', '')}\n")

        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        sequenced_results = None
        body_override = None

        if "/v5/search/" in parsed.path:
            dependency = unquote(parsed.path.rsplit("/", 1)[-1])
            names = fixture.get("providers", {}).get(dependency, [])
            response_type = "search"
            response_override = fixture.get("search_response_overrides", {}).get(
                dependency
            )
            response_status = fixture.get("search_status_overrides", {}).get(
                dependency, 200
            )
        else:
            names = query.get("arg[]", [])
            response_type = "multiinfo"
            requested_name = names[0] if len(names) == 1 else None
            response_status = fixture.get("info_status_overrides", {}).get(
                requested_name, 200
            )
            body_override = fixture.get("info_body_overrides", {}).get(
                requested_name
            )
            response_override = fixture.get("info_response_overrides", {}).get(
                requested_name
            )
            if len(names) == 1:
                sequenced_results = sequenced_info_results(
                    requested_name, fixture.get("packages", {})
                )
                if sequenced_results is None:
                    names = fixture.get("info_results", {}).get(requested_name, names)

        packages = fixture.get("packages", {})
        if sequenced_results is not None:
            results = sequenced_results
        else:
            results = [packages[name] for name in names if name in packages]
        response = {
            "version": 5,
            "type": response_type,
            "resultcount": len(results),
            "results": results,
        }
        response = apply_response_override(response, response_override)
        body = json.dumps(response).encode("utf-8")
        if body_override is not None:
            body = body_override.encode("utf-8")
        self.send_response(response_status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        return


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
port_path.write_text(str(server.server_port), encoding="utf-8")
server.serve_forever()
