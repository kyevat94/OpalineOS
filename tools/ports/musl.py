# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The OpalineOS authors
"""Port of musl, the C library of OpalineOS (MIT license).

Builds musl unmodified for x86-64 Linux: thanks to the mixed ABI (project.md section 3.1) the
OpalineOS kernel answers the same system calls. Python replaces musl's GNU make build, which
Windows lacks; the logic follows musl's Makefile:
  * every src/*/*.c is compiled, unless src/*/x86_64/ has a .s/.S/.c with the same name;
  * bits/alltypes.h and bits/syscall.h are generated from arch/x86_64;
  * libc.a from the normal objects, libc.so (which is also the dynamic linker,
    /lib/ld-musl-x86_64.so.1) from position-independent ones plus ldso/.

    python tools/ports/musl.py            # downloads (once), verifies and builds into build/sysroot

Output (build/sysroot): include/, lib/{crt1.o,crti.o,crtn.o,Scrt1.o,rcrt1.o,libc.a,libc.so}.
The source tarball is checked against the SHA-256 recorded here (its GPG signature by the musl
key 8364 8929 0BB6 B70F 99FF DA05 56BC DB59 3020 450F was verified when the hash was recorded).
"""
import concurrent.futures
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

VERSION = "1.2.6"
URL = f"https://musl.libc.org/releases/musl-{VERSION}.tar.gz"
SHA256 = "d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a"
ARCH = "x86_64"
LDSO = "/lib/ld-musl-x86_64.so.1"

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PORT = os.path.join(ROOT, "ports", "musl")
WORK = os.path.join(ROOT, "build", "ports")
SRC = os.path.join(WORK, "src", f"musl-{VERSION}")
OBJ = os.path.join(WORK, "obj", f"musl-{VERSION}")
SYSROOT = os.path.join(ROOT, "build", "sysroot")
LLVM = r"C:\Program Files\LLVM\bin"

CFLAGS = [
    "--target=x86_64-unknown-linux-musl", "-std=c99", "-nostdinc", "-ffreestanding",
    "-frounding-math", "-fno-strict-aliasing", "-Wa,--noexecstack",
    "-D_XOPEN_SOURCE=700", "-Os", "-pipe", "-fomit-frame-pointer",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-ffunction-sections", "-fdata-sections",
    "-w", "-Qunused-arguments",
    "-Werror=implicit-function-declaration", "-Werror=implicit-int", "-Werror=pointer-sign",
    "-Werror=pointer-arith", "-Werror=int-conversion", "-Werror=incompatible-pointer-types",
]
NOSSP = ("__libc_start_main", "__init_tls", "__stack_chk_fail", "__set_thread_area", "memset", "memcpy")


def tool(name):
    path = os.path.join(LLVM, name + ".exe")
    if not os.path.isfile(path):
        sys.exit(f"ERROR: {path} not found (install LLVM)")
    return path


def fetch_source():
    if os.path.isdir(SRC):
        return
    archive = os.path.join(WORK, "downloads", f"musl-{VERSION}.tar.gz")
    if not os.path.isfile(archive):
        os.makedirs(os.path.dirname(archive), exist_ok=True)
        print(f"  downloading {URL}", flush=True)
        urllib.request.urlretrieve(URL, archive)
    with open(archive, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    if digest != SHA256:
        sys.exit(f"ERROR: {archive} has SHA-256 {digest}, expected {SHA256}")
    with tarfile.open(archive) as t:
        t.extractall(os.path.dirname(SRC), filter="data")
    for patch in sorted(os.listdir(os.path.join(PORT, "patches"))):
        print(f"  applying {patch}", flush=True)
        with open(os.path.join(PORT, "patches", patch), "rb") as f:
            r = subprocess.run(["patch", "-p1", "--batch", "--forward"], cwd=SRC, stdin=f, capture_output=True)
        if r.returncode:
            sys.exit(f"ERROR: patch {patch} failed:\n{r.stdout.decode()}{r.stderr.decode()}")


def generate_headers():
    inc = os.path.join(OBJ, "include", "bits")
    os.makedirs(inc, exist_ok=True)
    os.makedirs(os.path.join(OBJ, "src", "internal"), exist_ok=True)
    # bits/alltypes.h: tools/mkalltypes.sed applied to the arch and generic templates
    out = []
    for name in (os.path.join(SRC, "arch", ARCH, "bits", "alltypes.h.in"), os.path.join(SRC, "include", "alltypes.h.in")):
        for line in open(name, encoding="utf-8"):
            line = line.rstrip("\n")
            m = re.match(r"^TYPEDEF (.*) ([^ ]*);$", line)
            if m:
                out += [f"#if defined(__NEED_{m[2]}) && !defined(__DEFINED_{m[2]})", f"typedef {m[1]} {m[2]};",
                        f"#define __DEFINED_{m[2]}", "#endif", ""]
                continue
            m = re.match(r"^(STRUCT|UNION) *([^ ]*) (.*);$", line)
            if m:
                kind = m[1].lower()
                out += [f"#if defined(__NEED_{kind}_{m[2]}) && !defined(__DEFINED_{kind}_{m[2]})", f"{kind} {m[2]} {m[3]};",
                        f"#define __DEFINED_{kind}_{m[2]}", "#endif", ""]
                continue
            out.append(line)
    write(os.path.join(inc, "alltypes.h"), "\n".join(out) + "\n")
    # bits/syscall.h: the template plus SYS_* aliases of every __NR_*
    text = open(os.path.join(SRC, "arch", ARCH, "bits", "syscall.h.in"), encoding="utf-8").read()
    aliases = [line.replace("__NR_", "SYS_") for line in text.splitlines() if "__NR_" in line]
    write(os.path.join(inc, "syscall.h"), text + "\n".join(aliases) + "\n")
    write(os.path.join(OBJ, "src", "internal", "version.h"), f'#define VERSION "{VERSION}"\n')


def write(path, text):
    if os.path.isfile(path) and open(path, encoding="utf-8").read() == text:
        return
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def sources():
    """(source, object stem, kind) for libc, crt and ldso, with arch files replacing generic ones."""
    dirs = [os.path.join("src", d) for d in sorted(os.listdir(os.path.join(SRC, "src")))
            if os.path.isdir(os.path.join(SRC, "src", d))]
    dirs += [os.path.join("src", "malloc", "mallocng"), "crt", "ldso"]
    chosen = {}
    for d in dirs:
        base = os.path.join(SRC, d)
        for f in sorted(os.listdir(base)):
            if f.endswith(".c"):
                chosen[(d, f[:-2])] = os.path.join(d, f)
        archdir = os.path.join(base, ARCH)
        if os.path.isdir(archdir):
            for f in sorted(os.listdir(archdir)):
                stem, ext = os.path.splitext(f)
                if ext in (".s", ".S", ".c"):
                    chosen[(d, stem)] = os.path.join(d, ARCH, f)   # replaces the generic file
    out = []
    for (d, stem), rel in sorted(chosen.items()):
        kind = "crt" if d == "crt" else "ldso" if d == "ldso" else "libc"
        out.append((rel, os.path.join(d, stem), kind, stem))
    return out


def compile_one(job):
    rel, stem_path, pic, extra, out = job
    src = os.path.join(SRC, rel)
    if os.path.isfile(out) and os.path.getmtime(out) >= os.path.getmtime(src):
        return None
    os.makedirs(os.path.dirname(out), exist_ok=True)
    inc = ["-I", os.path.join(SRC, "arch", ARCH), "-I", os.path.join(SRC, "arch", "generic"),
           "-I", os.path.join(OBJ, "src", "internal"), "-I", os.path.join(SRC, "src", "include"),
           "-I", os.path.join(SRC, "src", "internal"), "-I", os.path.join(OBJ, "include"),
           "-I", os.path.join(SRC, "include")]
    cmd = [tool("clang")] + CFLAGS + inc + extra + (["-fPIC"] if pic else []) + ["-c", "-o", out, src]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        return f"{rel}:\n{r.stdout}{r.stderr}"
    return None


def build():
    fetch_source()
    generate_headers()
    jobs, libc_o, libc_lo, ldso_lo, crt = [], [], [], [], {}
    for rel, stem_path, kind, stem in sources():
        extra = []
        if kind in ("crt", "ldso") or stem in NOSSP:
            extra.append("-fno-stack-protector")
        if kind == "crt":
            extra.append("-DCRT")
        if stem in ("memcpy", "memmove", "memcmp", "memset"):
            extra.append("-fno-builtin")
        base = os.path.join(OBJ, stem_path)
        if kind == "crt":
            pic = stem in ("Scrt1", "rcrt1")
            jobs.append((rel, stem_path, pic, extra, base + ".o"))
            crt[stem] = base + ".o"
        elif kind == "ldso":
            jobs.append((rel, stem_path, True, extra, base + ".lo"))
            ldso_lo.append(base + ".lo")
        else:
            jobs.append((rel, stem_path, False, extra, base + ".o"))
            jobs.append((rel, stem_path, True, extra, base + ".lo"))
            libc_o.append(base + ".o")
            libc_lo.append(base + ".lo")

    # OpalineOS's own compiler runtime (ports/rt): what libgcc/compiler-rt provide on Linux
    rt_objs = []
    rt_dir = os.path.join(ROOT, "ports", "rt")
    for f in sorted(os.listdir(rt_dir)):
        if f.endswith(".c"):
            out = os.path.join(OBJ, "rt", f[:-2] + ".lo")
            jobs.append((os.path.relpath(os.path.join(rt_dir, f), SRC), "rt/" + f[:-2], True, ["-O2"], out))
            rt_objs.append(out)

    print(f"  compiling {len(jobs)} objects", flush=True)
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for err in pool.map(compile_one, jobs):
            if err:
                errors.append(err)
    if errors:
        sys.exit("ERROR: musl compilation failed:\n" + "\n".join(errors[:10]))

    lib = os.path.join(SYSROOT, "lib")
    os.makedirs(lib, exist_ok=True)
    archive = os.path.join(lib, "libc.a")
    if os.path.isfile(archive):
        os.remove(archive)
    rsp = os.path.join(OBJ, "libc_a.rsp")
    write(rsp, "\n".join(o.replace("\\", "/") for o in libc_o) + "\n")
    run([tool("llvm-ar"), "rcs", archive, "@" + rsp])

    rt_archive = os.path.join(lib, "libopaline_rt.a")
    if os.path.isfile(rt_archive):
        os.remove(rt_archive)
    run([tool("llvm-ar"), "rcs", rt_archive] + rt_objs)

    rsp = os.path.join(OBJ, "libc_so.rsp")
    write(rsp, "\n".join(o.replace("\\", "/") for o in libc_lo + ldso_lo + rt_objs) + "\n")
    run([tool("ld.lld"), "-shared", "-nostdlib", "-e", "_dlstart", "--sort-section=alignment", "--sort-common",
         "--gc-sections", "--hash-style=both", "--no-undefined", "--exclude-libs=ALL",
         f"--dynamic-list={os.path.join(SRC, 'dynamic.list')}", "-soname", "libc.so",
         "-o", os.path.join(lib, "libc.so"), "@" + rsp])

    for name in ("crt1", "crti", "crtn", "Scrt1", "rcrt1"):
        shutil.copyfile(crt[name], os.path.join(lib, name + ".o"))
    for name in ("m", "rt", "pthread", "crypt", "util", "xnet", "resolv", "dl"):   # all inside libc
        empty = os.path.join(lib, f"lib{name}.a")
        if not os.path.isfile(empty):
            run([tool("llvm-ar"), "rcs", empty])

    install_headers()
    print(f"  OK: musl {VERSION} in {os.path.relpath(SYSROOT, ROOT)} ({len(libc_o)} objects)", flush=True)


def install_headers():
    dest = os.path.join(SYSROOT, "include")
    for src_dir in (os.path.join(SRC, "include"), os.path.join(SRC, "arch", "generic"), os.path.join(SRC, "arch", ARCH),
                    os.path.join(OBJ, "include")):
        for dirpath, _, files in os.walk(src_dir):
            for f in files:
                if not f.endswith(".h"):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, f), src_dir)
                out = os.path.join(dest, rel)
                os.makedirs(os.path.dirname(out), exist_ok=True)
                shutil.copyfile(os.path.join(dirpath, f), out)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"ERROR: {' '.join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd))}\n{r.stdout}{r.stderr}")


if __name__ == "__main__":
    build()
