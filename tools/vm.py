# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The OpalineOS authors
"""Runs and tests OpalineOS in QEMU and VirtualBox.

    python tools/vm.py run [qemu|vbox] [--cmdline "..."]   # open a window (default: qemu)
    python tools/vm.py test [qemu|vbox|all]                # automated boot tests (default: all)
    python tools/vm.py screenshot OUT.png [--cmdline "..."] # QEMU screenshot once the kernel is up

Each test boots an image headless, reads the serial port (COM1) and checks
for expected and forbidden text. It prints TEST_OK / TEST_FAILED per test.
Only machines started by this script are stopped.
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build  # noqa: E402

ROOT = build.ROOT
BUILD = build.BUILD
IMAGE = build.IMAGE
VMDK = build.VMDK

QEMU_DIRS = [r"C:\Program Files\qemu"]
VBOX_DIRS = [r"C:\Program Files\Oracle\VirtualBox"]
VM_NAME = "OpalineOS"
TIMEOUT = 90          # seconds per test

# (name, emulator, command line or None, text that must appear, text that must not appear)
TESTS = [
    ("boot", "qemu", None,
     ["Hello, OpalineOS!", "Olá, mundo! Ação", "GDT, TSS and IDT loaded.",
      "Breakpoint (#BP)", "TEST_OK E1 kernel"],
     ["PANIC"]),
    ("page-fault", "qemu", "test=pagefault",
     ["PANIC: Page fault (#PF)", "CR2=0xffffc00000000000", "not-present page on read in kernel mode",
      "Stack trace:", "The system has stopped."],
     ["TEST_OK E1 kernel"]),
    ("invalid-opcode", "qemu", "test=ud",
     ["PANIC: Invalid opcode (#UD)", "The system has stopped."],
     ["TEST_OK E1 kernel"]),
    ("divide-error", "qemu", "test=divide",
     ["PANIC: Divide error (#DE)", "The system has stopped."],
     ["TEST_OK E1 kernel"]),
    ("boot", "vbox", None,
     ["Hello, OpalineOS!", "Olá, mundo! Ação", "Breakpoint (#BP)", "TEST_OK E1 kernel"],
     ["PANIC"]),
]
DONE_MARKERS = ["TEST_OK E1 kernel", "The system has stopped."]


def find(name, dirs):
    found = shutil.which(name)
    if found:
        return found
    for d in dirs:
        cand = os.path.join(d, name + ".exe")
        if os.path.isfile(cand):
            return cand
    sys.exit(f"ERROR: '{name}' not found.")


def need_image():
    if not os.path.isfile(IMAGE):
        sys.exit("ERROR: build/opaline.img not found. Run: python tools/build.py")


def image_for(cmdline):
    """The normal image, or a variant with CMDLINE.TXT."""
    need_image()
    if cmdline is None:
        return IMAGE
    safe = "".join(c if c.isalnum() else "-" for c in cmdline).strip("-")
    path = os.path.join(BUILD, f"variant-{safe}.img")
    build.make_image(path, cmdline)
    return path


def read_log(log):
    if not os.path.isfile(log):
        return ""
    with open(log, "rb") as f:
        return f.read().decode("utf-8", errors="replace")


def wait_for_done(log, alive):
    """Waits until the kernel reports it finished (or panicked), the machine stops, or time runs out."""
    deadline = time.time() + TIMEOUT
    while time.time() < deadline:
        text = read_log(log)
        if any(m in text for m in DONE_MARKERS):
            time.sleep(0.5)          # let the rest of the output arrive
            return read_log(log)
        if not alive():
            return read_log(log)
        time.sleep(0.25)
    return read_log(log)


# ---- QEMU ---------------------------------------------------------------

def qemu_cmd(image, serial_log, headless, extra=()):
    qemu = find("qemu-system-x86_64", QEMU_DIRS)
    share = os.path.join(os.path.dirname(qemu), "share")
    code = os.path.join(share, "edk2-x86_64-code.fd")
    vars_template = os.path.join(share, "edk2-i386-vars.fd")   # the x86_64 firmware uses this vars layout
    vars_copy = os.path.join(BUILD, "ovmf-vars.fd")
    if not os.path.isfile(vars_copy):
        shutil.copyfile(vars_template, vars_copy)
    cmd = [qemu, "-machine", "q35", "-m", "512", "-no-reboot",
           "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
           "-drive", f"if=pflash,format=raw,file={vars_copy}",
           "-drive", f"format=raw,file={image}",
           "-serial", f"file:{serial_log}", *extra]
    if headless:
        cmd += ["-display", "none"]
    return cmd


def qemu_boot(image, log, extra=()):
    if os.path.isfile(log):
        os.remove(log)
    return subprocess.Popen(qemu_cmd(image, log, headless=True, extra=extra),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def qemu_test(cmdline):
    log = os.path.join(BUILD, "serial-qemu.log")
    proc = qemu_boot(image_for(cmdline), log)
    try:
        return wait_for_done(log, lambda: proc.poll() is None), log
    finally:
        if proc.poll() is None:
            proc.kill()            # our own QEMU process, by handle
        proc.wait()


def qemu_run(cmdline):
    log = os.path.join(BUILD, "serial-qemu.log")
    subprocess.Popen(qemu_cmd(image_for(cmdline), log, headless=False))
    print(f"QEMU started. Serial log: {os.path.relpath(log, ROOT)}")


def screenshot(out, cmdline):
    log = os.path.join(BUILD, "serial-shot.log")
    port = 45454
    proc = qemu_boot(image_for(cmdline), log, extra=["-monitor", f"tcp:127.0.0.1:{port},server,nowait"])
    try:
        wait_for_done(log, lambda: proc.poll() is None)
        time.sleep(1.0)
        with socket.create_connection(("127.0.0.1", port)) as s:
            time.sleep(0.3)
            s.sendall(f"screendump {os.path.abspath(out).replace(os.sep, '/')} -f png\n".encode())
            time.sleep(1.5)
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()
    print(f"OK: {out}" if os.path.isfile(out) else "ERROR: no screenshot written")


# ---- VirtualBox ---------------------------------------------------------

def vbox(*args, check=True):
    exe = find("VBoxManage", VBOX_DIRS)
    result = subprocess.run([exe, *args], capture_output=True, text=True)
    if check and result.returncode != 0:
        sys.exit(f"ERROR: VBoxManage {' '.join(args)}\n{result.stderr.strip()}")
    return result.stdout


def vbox_running():
    return f'"{VM_NAME}"' in vbox("list", "runningvms")


def vbox_prepare(serial_log):
    need_image()
    if not os.path.isfile(VMDK):
        sys.exit("ERROR: build/opaline.vmdk not found. Run: python tools/build.py")
    if f'"{VM_NAME}"' not in vbox("list", "vms"):
        print(f"Creating VirtualBox VM '{VM_NAME}'...")
        vbox("createvm", "--name", VM_NAME, "--ostype", "Other_64", "--register")
        vbox("modifyvm", VM_NAME, "--firmware", "efi", "--memory", "512", "--cpus", "1",
             "--graphicscontroller", "vmsvga", "--vram", "32", "--audio-enabled", "off",
             "--nic1", "none", "--boot1", "disk", "--boot2", "none", "--boot3", "none", "--boot4", "none")
        vbox("storagectl", VM_NAME, "--name", "SATA", "--add", "sata", "--controller", "IntelAhci",
             "--portcount", "1")
        vbox("storageattach", VM_NAME, "--storagectl", "SATA", "--port", "0", "--device", "0",
             "--type", "hdd", "--medium", VMDK)
    vbox("modifyvm", VM_NAME, "--uart1", "0x3F8", "4", "--uart-mode1", "file", serial_log)


def vbox_run(cmdline):
    if cmdline is not None:
        sys.exit("ERROR: --cmdline is only supported with QEMU")
    log = os.path.join(BUILD, "serial-vbox.log")
    if vbox_running():
        print(f"Powering off the running '{VM_NAME}' VM...")
        vbox("controlvm", VM_NAME, "poweroff")
        time.sleep(2)
    vbox_prepare(log)
    vbox("startvm", VM_NAME, "--type", "gui")
    print(f"VirtualBox started. Serial log: {os.path.relpath(log, ROOT)}")


def vbox_test(cmdline):
    assert cmdline is None, "VirtualBox tests use the normal image"
    log = os.path.join(BUILD, "serial-vbox.log")
    if vbox_running():
        sys.exit(f"ERROR: the '{VM_NAME}' VM is already running. Close it and run the test again.")
    vbox_prepare(log)
    if os.path.isfile(log):
        os.remove(log)
    vbox("startvm", VM_NAME, "--type", "headless")
    try:
        return wait_for_done(log, vbox_running), log
    finally:
        if vbox_running():
            vbox("controlvm", VM_NAME, "poweroff", check=False)


# ---- main ---------------------------------------------------------------

def run_tests(target):
    runners = {"qemu": qemu_test, "vbox": vbox_test}
    failed = 0
    for name, emu, cmdline, expect, forbid in TESTS:
        if target not in ("all", emu):
            continue
        text, log = runners[emu](cmdline)
        missing = [e for e in expect if e not in text]
        present = [f for f in forbid if f in text]
        ok = not missing and not present
        failed += not ok
        print(f"{'TEST_OK' if ok else 'TEST_FAILED'} {emu}/{name}", flush=True)
        for m in missing:
            print(f"    missing:    {m!r}")
        for p in present:
            print(f"    unexpected: {p!r}")
        if not ok:
            keep = os.path.join(BUILD, f"failed-{emu}-{name}.log")
            shutil.copyfile(log, keep) if os.path.isfile(log) else None
            print(f"    serial log: {os.path.relpath(keep, ROOT)}")
    return failed


def main():
    ap = argparse.ArgumentParser(description="Run and test OpalineOS")
    sub = ap.add_subparsers(dest="action", required=True)
    r = sub.add_parser("run")
    r.add_argument("target", nargs="?", default="qemu", choices=["qemu", "vbox"])
    r.add_argument("--cmdline")
    t = sub.add_parser("test")
    t.add_argument("target", nargs="?", default="all", choices=["qemu", "vbox", "all"])
    s = sub.add_parser("screenshot")
    s.add_argument("out")
    s.add_argument("--cmdline")
    args = ap.parse_args()

    if args.action == "run":
        (qemu_run if args.target == "qemu" else vbox_run)(args.cmdline)
    elif args.action == "screenshot":
        screenshot(args.out, args.cmdline)
    else:
        sys.exit(1 if run_tests(args.target) else 0)


if __name__ == "__main__":
    main()
