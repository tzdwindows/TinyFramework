"""Extract the mini_js_shim C string from mini_js_bridge.c into a .js file
and run `node --check` to find the exact syntax-error line."""
import os
import re
import subprocess

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src", "mini_js_bridge.c")
OUT = os.path.join(ROOT, "build", "shim.js")


def extract():
    txt = open(SRC, encoding="utf-8", errors="replace").read()
    m = re.search(r'static const char \*mini_js_shim\s*=\s*\n(.*?)(?=\n[a-zA-Z_])',
                  txt, re.S)
    if not m:
        # fall back: from the marker to the closing `"\n    "\n...` — take until
        # a line that is just whitespace after a string. Grab the string block.
        m = re.search(r'static const char \*mini_js_shim\s*=\s*\n(.*?)\n\;\s*\n',
                      txt, re.S)
    block = m.group(1) if m else ""
    js = []
    for line in block.splitlines():
        s = line.strip()
        if not (s.startswith('"') and '",' in s):
            # tolerate lines that end with `"` (last line of the string may not have comma)
            if s.startswith('"') and s.endswith('"'):
                js.append(s[1:-1])
            continue
        # strip surrounding " ..." , → keep inner
        inner = s[1:s.rfind('"')]
        js.append(inner)
    out = "\n".join(js)
    # unescape C string escapes that the compiler/eval would interpret
    out = out.replace("\\n", "\n").replace('\\"', '"').replace("\\'", "'")
    # leave \\ as-is (valid in JS regex) but turn \\' handling already done
    open(OUT, "w", encoding="utf-8").write(out)
    print("wrote", OUT, len(out), "chars")


def check():
    r = subprocess.run(["node", "--check", OUT],
                       capture_output=True, text=True)
    print("node --check rc=", r.returncode)
    print(r.stderr or r.stdout)


if __name__ == "__main__":
    extract()
    check()
