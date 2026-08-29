"""
debug_leak.py — find which TinyFramework surface leaks a JS object at runtime
teardown (the quickjs.c:2732 `list_empty(&rt->gc_obj_list)` assertion).

Strategy: the CRT assertion pops a modal dialog and BLOCKS the process, so a
run that exits cleanly within the timeout = no leak; a run that TIMES OUT =
the assertion fired during mini_app_destroy (a leaked JSValue survived
JS_FreeRuntime). We exercise each surface in isolation to pinpoint which one
leaks. Each test HTML keeps a requestAnimationFrame loop so a TINY_FRAMES cap
drives a normal exit (and thus the teardown path where the assertion fires).

Kill any stale tiny_app.exe / WerFault.exe between runs so a hung popup from
one test doesn't taint the next.
"""
import os
import subprocess
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(ROOT, "build", "tiny_app.exe")

# (name, entry html, frames) — each isolates one surface built in this change.
TESTS = [
    ("baseline (primary only, no new surface)", "build/only_rAF.html", 120),
    ("multi-window + loadFile + window-state", "build/test_multiwin.html", 150),
    ("IPC send/invoke/on + webContents.send", "build/test_ipc.html", 180),
    ("app lifecycle/paths/single-instance", "build/test_app.html", 120),
    ("protocol app:// + session + net.request", "build/test_proto.html", 200),
    ("FULL demo (all surfaces together)", "build/demo_full.html", 260),
]


def kill_stale():
    for name in ("tiny_app", "WerFault"):
        subprocess.run(
            ["powershell", "-Command",
             "Get-Process %s -ErrorAction SilentlyContinue | Stop-Process -Force" % name],
            capture_output=True)
    time.sleep(0.5)


def run_one(name, entry, frames, timeout_s=20.0):
    kill_stale()
    env = dict(os.environ)
    env["TINY_FRAMES"] = str(frames)
    env["TINY_DUMP_LEAKS"] = "1"   # enable QuickJS JS_DUMP_LEAKS at teardown
    env["TINY_LOG_FILE"] = ""     # so we don't spam the shared log mid-batch
    try:
        p = subprocess.run(
            [EXE, os.path.join(ROOT, entry)],
            cwd=ROOT, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout_s, creationflags=0x08000000)  # CREATE_NO_WINDOW
        out = p.stdout.decode("utf-8", "replace")
        err = p.stderr.decode("utf-8", "replace")
        dump = out or err
        if p.returncode == 0:
            return ("exit 0 (clean)", True, dump)
        # non-zero exit => abort (assertion). mini_crash wrote a dump too.
        snippet = _leak_snippet(dump)
        return ("exit %d (ABORT/leak)" % p.returncode, False,
                "  --- captured stdout/stderr ---\n%s" % (snippet or dump[:2000]))
    except subprocess.TimeoutExpired:
        kill_stale()
        return ("TIMEOUT (>%.0fs -> still a blocking popup -> LEAK)" % timeout_s, False, "")


def _leak_snippet(text):
    """Pull the 'Object leaks:' block out of QuickJS's dump for a compact view."""
    if not text:
        return ""
    lines = text.splitlines()
    out = []
    cap = 60
    for i, ln in enumerate(lines):
        if "Object leaks" in ln or "Secondary object leaks" in ln or "leak" in ln.lower():
            out.extend(lines[i:i + cap])
            break
    return "\n".join(out) if out else ""


def main():
    print("TinyFramework leak probe — quickjs.c:2732 list_empty(&rt->gc_obj_list)")
    print("EXE:", EXE, "| exists:", os.path.exists(EXE))
    print("DUMP: TINY_DUMP_LEAKS=1 -> JS_DUMP_LEAKS; assert -> stderr+abort (no popup)")
    print("-" * 72)
    results = []
    for name, entry, frames in TESTS:
        path = os.path.join(ROOT, entry)
        if not os.path.exists(path):
            print("  [skip] %-48s (missing %s)" % (name, entry))
            continue
        outcome, ok, detail = run_one(name, entry, frames)
        flag = "OK  " if ok else "LEAK"
        print("  [%s] %-44s %s" % (flag, name, outcome))
        if detail:
            print(detail)
            print("-" * 72)
        results.append((name, ok))
    kill_stale()
    print("-" * 72)
    leaks = [n for n, ok in results if not ok]
    if not leaks:
        print("RESULT: no leaks detected — the runtime teardown is clean on every surface.")
    else:
        print("RESULT: LEAK on: %s" % ", ".join(leaks))


if __name__ == "__main__":
    main()
