"""Push tracking_map.flow.json into a running Node-RED instance via Admin API.

Replaces all nodes belonging to the map_tab tab + its page/groups, so re-importing
in the editor isn't needed. Just refresh the dashboard after the script runs.
"""
import json
import sys
import urllib.request
import urllib.error

NODE_RED = "http://localhost:1880"
FLOW_FILE = "tracking_map.flow.json"

# IDs that belong to the tracking_map flow (tab + page + groups + nodes with z=map_tab)
STATIC_IDS = {"map_tab", "map_ui_page", "map_grp_canvas", "map_grp_anchor"}


def http(method, path, data=None, extra_headers=None):
    url = NODE_RED + path
    body = json.dumps(data).encode() if data is not None else None
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("Content-Type", "application/json")
    if extra_headers:
        for k, v in extra_headers.items():
            req.add_header(k, v)
    with urllib.request.urlopen(req) as r:
        return r.status, dict(r.headers), r.read()


def main():
    # 1. Load the new map flow
    with open(FLOW_FILE, encoding="utf-8") as f:
        new_nodes = json.load(f)
    print(f"[+] Loaded {len(new_nodes)} nodes from {FLOW_FILE}")

    # 2. GET current flows (and revision)
    status, headers, body = http("GET", "/flows")
    if status != 200:
        sys.exit(f"GET /flows -> {status}")
    current = json.loads(body)
    if isinstance(current, dict):
        rev = current.get("rev")
        flows = current.get("flows", [])
    else:
        rev = headers.get("Node-RED-Deployment-Rev") or headers.get("node-red-deployment-rev")
        flows = current
    print(f"[+] Current flow has {len(flows)} nodes (rev={rev})")

    # 3. Strip out everything belonging to map_tab
    before = len(flows)
    filtered = [n for n in flows if n.get("id") not in STATIC_IDS and n.get("z") != "map_tab"]
    removed = before - len(filtered)
    print(f"[+] Removed {removed} existing map_tab nodes")

    # 4. Append the new nodes
    merged = filtered + new_nodes
    print(f"[+] Posting {len(merged)} total nodes")

    # 5. POST back as full deploy
    payload = {"flows": merged, "rev": rev} if rev else merged
    status, _, body = http(
        "POST", "/flows", payload,
        extra_headers={"Node-RED-Deployment-Type": "full"},
    )
    print(f"[+] POST /flows -> {status}")
    if status >= 300:
        print(body.decode(errors="replace"))
        sys.exit(1)
    print("[OK] Refresh the dashboard tab to pick up the new flow.")


if __name__ == "__main__":
    main()
