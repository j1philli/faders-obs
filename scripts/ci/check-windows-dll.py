#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Static checks for faders.dll: exports exactly what libobs looks up, and every symbol it imports
# from obs.dll / obs-frontend-api.dll / w32-pthreads.dll exists in the real release DLLs.
import re, shutil, subprocess, sys

dll, libdir = sys.argv[1], sys.argv[2]
readobj = shutil.which("llvm-readobj") or "/opt/homebrew/opt/llvm/bin/llvm-readobj"
run = lambda *a: subprocess.run([readobj, *a, dll], capture_output=True, text=True, check=True).stdout

exports = sorted(re.findall(r"Name: (\S+)", run("--coff-exports")))
expected = sorted(["obs_module_load", "obs_module_unload", "obs_module_set_pointer", "obs_module_ver",
                   "obs_module_set_locale", "obs_module_free_locale", "obs_module_get_string", "obs_module_description"])
problems = [] if exports == expected else [f"exports {exports}, expected {expected}"]

imports, current = {}, None
for line in run("--coff-imports").splitlines():
    m = re.match(r"\s*Name: (\S+\.dll)", line, re.I)
    if m:
        current = m.group(1).lower(); imports.setdefault(current, set()); continue
    m = re.match(r"\s*Symbol: (\S+)", line)
    if m and current:
        imports[current].add(m.group(1))
for name in ("obs", "obs-frontend-api", "w32-pthreads"):
    available = {l.strip() for l in open(f"{libdir}/{name}.def") if l.startswith("  ")}
    used = imports.get(f"{name}.dll", set())
    missing = sorted(used - available)
    print(f"{name}.dll: {len(used)} symbols used, missing: {missing or 'none'}")
    if missing: problems.append(f"{name}.dll lacks {missing}")
allowed = {"kernel32.dll", "winhttp.dll", "obs.dll", "obs-frontend-api.dll", "w32-pthreads.dll"}
extra = sorted(d for d in imports if d not in allowed and not d.startswith("api-ms-win-crt-") and d != "msvcrt.dll")
if extra: problems.append(f"unexpected DLL dependencies (would need shipping): {extra}")
print("exports:", ", ".join(exports))
if problems:
    print("FAIL:", *problems, sep="\n  "); sys.exit(1)
print("PASS")
