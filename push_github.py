#!/usr/bin/env python3
"""Push local files to GitHub via the Contents API (PUT), through local proxy."""
import base64
import json
import os
import sys
import urllib.request

TOKEN = os.environ["GH_TOKEN"]
REPO = "18934020489Xx/alioth-woa-drivers"
ROOT = r"G:\woa\woa-drivers"
PROXY = "http://127.0.0.1:7897"

FILES = [
    "README.md",
    ".github/workflows/build.yml",
    "qcfakepep/qcfakepep.c",
    "qcfakepep/qcfakepep.inf",
    "qcfakepep/qcfakepep.vcxproj",
    "aliothcpufreq/aliothcpufreq.c",
    "aliothcpufreq/aliothcpufreq.inf",
    "aliothcpufreq/aliothcpufreq.vcxproj",
    "DEPLOY.md",
]
if len(sys.argv) > 1:
    FILES = sys.argv[1:]

handler = urllib.request.ProxyHandler({"https": PROXY, "http": PROXY})
opener = urllib.request.build_opener(handler)

def api(path, payload):
    req = urllib.request.Request(
        f"https://api.github.com{path}",
        data=json.dumps(payload).encode(),
        headers={
            "Authorization": f"Bearer {TOKEN}",
            "Accept": "application/vnd.github+json",
            "User-Agent": "nova-push",
            "Content-Type": "application/json",
        },
        method="PUT",
    )
    with opener.open(req, timeout=60) as r:
        return json.loads(r.read())

failed = 0
for rel in FILES:
    full = os.path.join(ROOT, rel.replace("/", os.sep))
    with open(full, "rb") as f:
        content = base64.b64encode(f.read()).decode()
    ok = False
    for attempt in range(1, 6):
        try:
            api(f"/repos/{REPO}/contents/{rel}", {
                "message": f"add {rel}",
                "content": content,
                "branch": "main",
            })
            print(f"OK   {rel} (attempt {attempt})")
            ok = True
            break
        except Exception as e:
            print(f"  retry {attempt} {rel}: {e}")
            import time
            time.sleep(2 * attempt)
    if not ok:
        failed += 1
        print(f"FAIL {rel}")

sys.exit(1 if failed else 0)
