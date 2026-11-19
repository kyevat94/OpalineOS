# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The OpalineOS authors
"""Runs and tests OpalineOS in QEMU and VirtualBox.

    python tools/vm.py run [qemu|vbox]        # open a window (default: qemu)
    python tools/vm.py test [qemu|vbox|all]   # headless boot test (default: all)

The test boots build/opaline.img, waits for the "TEST_OK" marker on the
serial port (COM1) and prints TEST_OK / TEST_FAILED for each emulator.
It only stops machines it started itself.
"""
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
IMAGE = os.path.join(BUILD, "opaline.img")
VMDK = os.path.join(BUILD, "opaline.vmdk")

QEMU_DIRS = [r"C:\Program Files\qemu"]
VBOX_DIRS = [r"C:\Program Files\Oracle\VirtualBox"]
VM_NAME = "OpalineOS"
TEST_MARKER = "TEST_OK E0"
TEST_TIMEOUT = 90          # seconds


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


def wait_for_marker(log, timeout, alive):
    """Polls the serial log until the marker appears, the machine stops or time runs out."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.isfile(log):
            with open(log, "rb") as f:
                if TEST_MARKER.encode() in f.read():
                    return True
        if not alive():
            return False
        time.sleep(0.5)
    return False


# ---- QEMU ---------------------------------------------------------------

def qemu_cmd(serial_log, headless):
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
           "-drive", f"format=raw,file={IMAGE}",
           "-serial", f"file:{serial_log}"]
    if headless:
        cmd += ["-display", "none"]
    return cmd


def qemu_run():
    need_image()
    log = os.path.join(BUILD, "serial-qemu.log")
    subprocess.Popen(qemu_cmd(log, headless=False))
    print(f"QEMU started. Serial log: {log}")


def qemu_test():
    need_image()
    log = os.path.join(BUILD, "serial-qemu.log")
    if os.path.isfile(log):
        os.remove(log)
    proc = subprocess.Popen(qemu_cmd(log, headless=True),
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        ok = wait_for_marker(log, TEST_TIMEOUT, lambda: proc.poll() is None)
    finally:
        if proc.poll() is None:
            proc.kill()            # our own QEMU process, by handle
        proc.wait()
    return ok, log


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


def vbox_run():
    log = os.path.join(BUILD, "serial-vbox.log")
    if vbox_running():
        print(f"Powering off the running '{VM_NAME}' VM...")
        vbox("controlvm", VM_NAME, "poweroff")
        time.sleep(2)
    vbox_prepare(log)
    vbox("startvm", VM_NAME, "--type", "gui")
    print(f"VirtualBox started. Serial log: {log}")


def vbox_test():
    log = os.path.join(BUILD, "serial-vbox.log")
    if vbox_running():
        sys.exit(f"ERROR: the '{VM_NAME}' VM is already running. Close it and run the test again.")
    vbox_prepare(log)
    if os.path.isfile(log):
        os.remove(log)
    vbox("startvm", VM_NAME, "--type", "headless")
    try:
        ok = wait_for_marker(log, TEST_TIMEOUT, vbox_running)
    finally:
        if vbox_running():
            vbox("controlvm", VM_NAME, "poweroff", check=False)
    return ok, log


# ---- main ---------------------------------------------------------------

def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("run", "test"):
        sys.exit(__doc__)
    action = sys.argv[1]
    target = sys.argv[2] if len(sys.argv) > 2 else ("qemu" if action == "run" else "all")

    if action == "run":
        {"qemu": qemu_run, "vbox": vbox_run}[target]()
        return

    tests = {"qemu": qemu_test, "vbox": vbox_test}
    names = list(tests) if target == "all" else [target]
    failed = False
    for name in names:
        ok, log = tests[name]()
        print(f"{'TEST_OK' if ok else 'TEST_FAILED'} {name}  (serial log: {os.path.relpath(log, ROOT)})")
        failed |= not ok
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
