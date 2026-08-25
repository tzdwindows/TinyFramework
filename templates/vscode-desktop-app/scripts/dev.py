#!/usr/bin/env python3
# dev.py — TinyFramework Live Development Runner
import os, sys, subprocess, time

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE_EXE = os.path.join(PROJECT_DIR, "..", "..", "build", "tiny_app.exe")
if not os.path.exists(ENGINE_EXE):
    ENGINE_EXE = os.path.join(PROJECT_DIR, "bin", "tiny_app.exe")

if not os.path.exists(ENGINE_EXE):
    print(f"[Dev Error] TinyFramework engine executable not found: {ENGINE_EXE}")
    sys.exit(1)

print(f"[Dev] Launching Desktop App in live dev mode...")
print(f"[Dev] CDPServer listening on http://localhost:9222")
entry = os.path.join(PROJECT_DIR, "src", "index.html")

env = os.environ.copy()
env["TINY_CDP_PORT"] = "9222"
os.chdir(PROJECT_DIR)

proc = subprocess.Popen([ENGINE_EXE, entry], env=env)
try:
    proc.wait()
except KeyboardInterrupt:
    proc.terminate()
