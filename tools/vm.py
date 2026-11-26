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
BOOT_OK = "TEST_OK boot complete"
PANIC_END = "The system has stopped."

TESTS = [
    ("boot", "qemu", None,
     ["Hello, OpalineOS!", "Olá, mundo! Ação", "Breakpoint (#BP)", "no-execute on",
      "Memory self-test passed", "TEST_OK E2 memory",
      "Timer: local APIC", "Threads A and B alternating without yielding: ABAB",
      "Scheduler self-test passed", "TEST_OK E3 scheduler",
      "usertest: write() from ring 3 works", "usertest: all checks passed",
      "Olá do espaço de usuário! Hello from user space (3/3)",
      'Process "crashnull"', "crashed: Page fault (#PF)", "address 0x0000000000000000",
      'Process "crashpriv"', "crashed: General protection fault (#GP)",
      'Process "crashkernel"', "address 0xffffffff80000000",
      "The process was stopped (exit code 139). The system continues.",
      "User-space self-test passed", "TEST_OK E4 userspace",
      "Disk sata0:", "Root file system: EFS3", "persist:", "fstest: all checks passed", "TEST_OK E5 files",
      "posixtest (/apps/posixtest): musl", "posixtest (/apps/posixtest-static): musl",
      "POSIXTEST_OK /apps/posixtest-static", "POSIX self-test passed", "TEST_OK E6 posix",
      BOOT_OK],
     ["PANIC", "FAILED", "  FAIL ", "unexpected interrupt", "this must not happen"]),
    ("stack-overflow", "qemu", "test=stackoverflow",
     ["PANIC: Double fault (#DF)", 'Thread: "overflow"', "Cause: kernel stack overflow", PANIC_END],
     [BOOT_OK]),
    ("thread-fault", "qemu", "test=threadfault",
     ["PANIC: Page fault (#PF)", 'Thread: "faulty"', "CR2=0x0000000000000000", PANIC_END],
     [BOOT_OK]),
    ("page-fault", "qemu", "test=pagefault",
     ["PANIC: Page fault (#PF)", "CR2=0xffffc00000000000", "not-present page on read in kernel mode",
      "Stack trace:", PANIC_END],
     [BOOT_OK]),
    ("null-pointer", "qemu", "test=null",
     ["PANIC: Page fault (#PF)", "CR2=0x0000000000000000", "not-present page on read", PANIC_END],
     [BOOT_OK]),
    ("write-protect", "qemu", "test=wp",
     ["PANIC: Page fault (#PF)", "protection violation on write in kernel mode", PANIC_END],
     [BOOT_OK]),
    ("no-execute", "qemu", "test=nx",
     ["PANIC: Page fault (#PF)", "protection violation on instruction fetch", PANIC_END],
     [BOOT_OK]),
    ("double-free", "qemu", "test=doublefree",
     ["PANIC: kfree: double free of", PANIC_END],
     [BOOT_OK]),
    ("invalid-opcode", "qemu", "test=ud",
     ["PANIC: Invalid opcode (#UD)", PANIC_END],
     [BOOT_OK]),
    ("divide-error", "qemu", "test=divide",
     ["PANIC: Divide error (#DE)", PANIC_END],
     [BOOT_OK]),
    ("boot", "vbox", None,
     ["Hello, OpalineOS!", "Olá, mundo! Ação", "Memory self-test passed", "TEST_OK E2 memory",
      "Threads A and B alternating without yielding: ABAB", "Scheduler self-test passed",
      "TEST_OK E3 scheduler", "usertest: all checks passed", "User-space self-test passed",
      "TEST_OK E4 userspace", "Disk sata0:", "fstest: all checks passed", "TEST_OK E5 files",
      "POSIXTEST_OK /apps/posixtest-static", "POSIX self-test passed", "TEST_OK E6 posix", BOOT_OK,
      "ACPI: power off via PM1a control port", "Shutting down..."],
     ["PANIC", "FAILED", "  FAIL ", "unexpected interrupt", "this must not happen", "the VM did not power off"]),
]
POWERCUT_OK = "TEST_OK powercut"
DONE_MARKERS = [BOOT_OK, PANIC_END, POWERCUT_OK]


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
    """The normal image, or a variant with CMDLINE.TXT. All variants share one file
    (build/variant.img), so a test run does not leave a 128 MiB image per test behind."""
    need_image()
    if cmdline is None:
        return IMAGE
    for name in os.listdir(BUILD):   # images from older versions of this script
        if name.startswith("variant-") and (name.endswith(".img") or name.endswith(".cmdline.txt")):
            os.remove(os.path.join(BUILD, name))
    path = os.path.join(BUILD, "variant.img")
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
           "-drive", f"format=raw,file={image},cache=unsafe",   # fast flushes; power cuts are simulated in the guest
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


class QemuSession:
    """A headless QEMU with its monitor open: wait for serial output, type keys, take screenshots."""

    # characters the tests type -> QEMU key names (US layout)
    KEYS = {" ": "spc", "\n": "ret", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
            '"': "shift-apostrophe", "_": "shift-minus", ":": "shift-semicolon", "=": "equal",
            "$": "shift-4", "?": "shift-slash", "\t": "tab", "\b": "backspace"}

    def __init__(self, image, log_name="serial-session.log", port=None):
        # OPALINE_MONITOR_PORT lets several checkouts run their QEMU tests at the same time
        port = port or int(os.environ.get("OPALINE_MONITOR_PORT", "45455"))
        self.log = os.path.join(BUILD, log_name)
        self.port = port
        self.proc = qemu_boot(image, self.log, extra=["-monitor", f"tcp:127.0.0.1:{port},server,nowait"])
        self.sock = None

    def text(self):
        return read_log(self.log)

    def wait_for(self, needle, timeout=60, start=0):
        """Waits until `needle` appears in the serial output after position `start`."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            t = self.text()
            if needle in t[start:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def monitor(self, command):
        if not self.sock:
            self.sock = socket.create_connection(("127.0.0.1", self.port))
            time.sleep(0.3)
        self.sock.sendall((command + "\n").encode())

    def type(self, text):
        """Types text; "<name>" sends a special key by its QEMU name (<left>, <home>, <delete>...)."""
        i = 0
        while i < len(text):
            ch = text[i]
            if ch == "<" and ">" in text[i:]:
                end = text.index(">", i)
                key = text[i + 1:end]
                i = end + 1
            else:
                i += 1
                if ch.isalnum() and ch.isascii():
                    key = ("shift-" + ch.lower()) if ch.isupper() else ch
                else:
                    key = self.KEYS[ch]
            self.monitor(f"sendkey {key}")
            time.sleep(0.05)

    def screenshot(self, out):
        self.monitor(f"screendump {os.path.abspath(out).replace(os.sep, '/')} -f png")
        time.sleep(1.5)

    def pixels(self):
        """The screen as (width, height, RGB bytes), through a PPM screendump (no image library needed)."""
        path = os.path.join(BUILD, "screen.ppm")
        if os.path.isfile(path):
            os.remove(path)
        self.monitor(f"screendump {os.path.abspath(path).replace(os.sep, '/')}")
        for _ in range(50):
            time.sleep(0.1)
            if os.path.isfile(path) and os.path.getsize(path) > 100:
                break
        time.sleep(0.3)
        with open(path, "rb") as f:
            data = f.read()
        os.remove(path)
        parts = data.split(b"\n", 3)          # "P6", "W H", "255", pixels
        w, h = map(int, parts[1].split())
        return w, h, parts[3]

    def close(self):
        if self.sock:
            self.sock.close()
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()


# console geometry (kernel/console.c, assets/fonts/console.fnt)
CONSOLE_MARGIN, CELL_W, CELL_H = 16, 9, 18


def same_cells(screen, row_a, row_b, cells, cursor=None):
    """True if the first `cells` character cells of two console rows have identical pixels.
    Cell `cursor` of row_a may also be the inverse of row_b's (the blinking block cursor is drawn
    by inverting its cell: kernel/console.c)."""
    w, _, px = screen
    for dy in range(CELL_H):
        ya, yb = CONSOLE_MARGIN + row_a * CELL_H + dy, CONSOLE_MARGIN + row_b * CELL_H + dy
        xa = (ya * w + CONSOLE_MARGIN) * 3
        xb = (yb * w + CONSOLE_MARGIN) * 3
        if cursor is None:
            if px[xa:xa + cells * CELL_W * 3] != px[xb:xb + cells * CELL_W * 3]:
                return False
            continue
        c0, c1 = cursor * CELL_W * 3, (cursor + 1) * CELL_W * 3
        if px[xa:xa + c0] != px[xb:xb + c0] or px[xa + c1:xa + cells * CELL_W * 3] != px[xb + c1:xb + cells * CELL_W * 3]:
            return False
        a, b = px[xa + c0:xa + c1], px[xb + c0:xb + c1]
        if a != b and a != bytes(255 - v for v in b):
            return False
    return True


def shell_test():
    """Types commands into the shell and checks what it prints (on a fresh disk, so earlier runs
    cannot change the result)."""
    path = os.path.join(BUILD, "shell.img")
    build.make_image(path)
    s = QemuSession(path)
    try:
        if not s.wait_for("opaline:/home$ ", timeout=TIMEOUT):
            return False, s.text(), ["the shell prompt never appeared"]
        problems = []
        for keys, expect in [("echo ola mundo > nota.txt\n", "opaline:/home$ "), ("cat nota.txt\n", "ola mundo")]:
            mark = len(s.text())
            s.type(keys)
            if not s.wait_for(expect, timeout=30, start=mark) or not s.wait_for("$ ", timeout=10, start=mark):
                return False, s.text(), [f"after typing {keys.strip()!r}: {expect!r} not printed"]

        # Line editing: each sequence below must leave the line looking exactly like "cat nota.txt"
        # typed plainly (compared pixel by pixel with the same command two lines up: the screen
        # has scrolled, so the line being edited is the last one) and must then run that command.
        # Every sequence ends with the cursor at the end of the line; the cells after it (4 more)
        # are compared too, so text left over from a longer line shows, and the cursor's cell may
        # be inverted.
        edits = [
            ("Tab then Backspace in the middle of a word", "cat no\t\bta.txt"),
            ("Left arrows then Backspace", "cat notaa.txt<left><left><left><left>\b<end>"),
            ("Home then typing at the start", "at nota.txt<home>c<end>"),
            ("Left then Delete", "cat nota.txtZ<left><delete>"),
            ("Up arrow (history)", "<up>"),
            ("Tab in the middle, Left, Right and Delete", "cat nota<left><left>\t<left><right><left><delete><end>.txt"),
        ]
        typed = len("opaline:/home$ cat nota.txt")
        for what, keys in edits:
            s.type(keys)
            time.sleep(0.8)
            screen = s.pixels()
            rows = (screen[1] - 2 * CONSOLE_MARGIN) // CELL_H
            if not same_cells(screen, rows - 1, rows - 3, typed + 5, cursor=typed):
                problems.append(f"{what}: the edited line differs on screen from \"cat nota.txt\"")
            mark = len(s.text())
            s.type("\n")
            if not s.wait_for("ola mundo", timeout=30, start=mark) or not s.wait_for("$ ", timeout=10, start=mark):
                problems.append(f"{what}: the edited line did not run \"cat nota.txt\"")

        steps = [
            # Ctrl+C at the prompt throws the line away and gives a fresh prompt (the line must
            # not run: the next command shows only its own output)
            ("echo nao<ctrl-c>", "^C\r\nopaline:/home$ "),
            ("echo sim\n", "\r\nsim\r\n"),
            ("mkdir pasta\n", "opaline:/home$ "),
            ("ln -s nota.txt link.txt\n", "opaline:/home$ "),
            ("ls -l\n", "link.txt -> nota.txt"),
            ("cat link.txt\n", "ola mundo"),
            ("ls -l\n", "pasta/"),
            ("hello\n", "Hello from user space (3/3)"),
            ("crashnull\n", "[crashnull was stopped by signal 11]"),
            ("cd /apps\n", "opaline:/apps$ "),
            ("ls\n", "fstest"),
            ("cat /home/Documentos/nada\n", "ENOENT"),
            ("nosuchcmd\n", "nosuchcmd: command not found"),
            # job control: the program sleeps 300 s, so only the keys can bring the prompt back in time
            ("posixtest sleep\n", "sleeping..."),
            ("<ctrl-c>", "opaline:/apps$ "),
            ("posixtest sleep\n", "sleeping..."),
            ("<ctrl-z>", "[posixtest stopped; type fg to continue it]"),
            ("fg\n", "posixtest"),
            ("<ctrl-c>", "opaline:/apps$ "),
            # SE3: the environment reaches programs through fork + execve
            ("export GREETING=ola\n", "opaline:/apps$ "),
            ("posixtest env GREETING\n", "env GREETING=ola"),
            ("echo $GREETING $?\n", "ola 0"),
            ("FOO=bar posixtest env FOO\n", "env FOO=bar"),
            ("posixtest env FOO\n", "env FOO unset"),
            # raw mode: each key reaches the program at once, with no Enter
            ("posixtest rawkeys\n", "press keys, q quits"),
            ("a", "key 97"),
            ("b", "key 98"),
            ("<esc>", "key 27"),
            ("q", "rawkeys done"),
            # VMIN=3 VTIME=5: three keys typed quickly come back from one read()
            ("posixtest vmin\n", "type keys, q quits"),
            ("abc", "vmin got 3 bytes"),
            ("q", "vmin done"),
            # what was typed before tcsetattr(TCSAFLUSH) into raw mode is thrown away
            ("posixtest flushraw\n", "flushraw: type now"),
            ("xyz", "flushraw got 0 bytes"),
            # a program that leaves the terminal raw: the shell puts it back
            ("posixtest rawkeys\n", "press keys, q quits"),
            ("x", "rawkeys: leaving raw"),
            ("echo $GREETING-back\n", "ola-back"),
        ]
        for keys, expect in steps:
            mark = len(s.text())
            s.type(keys)
            if not s.wait_for(expect, timeout=30, start=mark):
                problems.append(f"after typing {keys.strip()!r}: {expect!r} not printed")
                break
        s.wait_for("opaline:/apps$ ", timeout=10, start=len(s.text()) - 200)
        s.screenshot(os.path.join(BUILD, "shell.png"))

        # leaving the main shell turns the computer off (ACPI): QEMU must exit by itself
        s.type("exit\n")
        deadline = time.time() + 20
        while s.proc.poll() is None and time.time() < deadline:
            time.sleep(0.2)
        if "Shutting down..." not in s.text():
            problems.append("\"exit\" did not print \"Shutting down...\"")
        if s.proc.poll() is None:
            problems.append("\"exit\" did not power the machine off (QEMU still running)")
        return not problems, s.text(), problems
    finally:
        s.close()
        os.remove(path)


def persistence_test():
    """Boots the same disk twice: the second boot must see what the first wrote. The disk is
    also read on the host (tools/efs3.py) and checked for consistency after each boot."""
    import efs3
    path = os.path.join(BUILD, "persist.img")
    build.make_image(path)   # a fresh disk
    problems, log = [], ""
    for boot, expect in ((1, "persist: first boot of this disk"), (2, "persist: found from a previous boot (boot 1)")):
        s = QemuSession(path, log_name="serial-persist.log")
        try:
            ok = s.wait_for(BOOT_OK, timeout=TIMEOUT)
            log = s.text()
        finally:
            s.close()
        if not ok or expect not in log:
            problems.append(f"boot {boot}: {expect!r} not printed")
            break
        fs = efs3.Efs3.from_disk(path, replay=False)   # after a clean run the journal must be empty
        counter = fs.read(fs.lookup("/home/persist.txt")).decode()
        if counter != f"{boot}\n":
            problems.append(f"boot {boot}: /home/persist.txt read on the host is {counter!r}, expected {boot!r}")
        errors = fs.check()
        problems += [f"boot {boot}: fsck: {e}" for e in errors[:5]]
    os.remove(path)
    return not problems, log, problems


def powercut_test():
    """Stage SE2: the disk loses power in the middle of a file system workload (the kernel drops every
    block write after the N-th: "test=powercut:N"); the next normal boot must repair it from the EFS3
    journal, and the host's fsck must then find nothing wrong. N runs over the whole workload."""
    import re
    import efs3
    path = os.path.join(BUILD, "powercut.img")
    log = os.path.join(BUILD, "serial-powercut.log")
    problems, texts = [], []

    def boot(img):
        proc = qemu_boot(img, log)
        try:
            return wait_for_done(log, lambda: proc.poll() is None)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()

    def cut_and_repair(n):
        cmd = f"test=powercut:{n}"
        build.make_image(path, cmd)
        text1 = boot(path)
        texts.append(text1)
        if POWERCUT_OK not in text1:
            return False, [f"cut after {n} writes: the workload did not reach its end"], text1
        with open(path, "r+b") as f:   # the second boot is a normal one: blank out the command line
            data = f.read()
            at = data.find(cmd.encode())
            if at < 0:
                return False, [f"cut after {n} writes: the command line is not in the image"], text1
            f.seek(at)
            f.write(b" " * len(cmd))
        text2 = boot(path)
        texts.append(text2)
        if BOOT_OK not in text2:
            return False, [f"cut after {n} writes: the next boot did not complete"], text2
        errors = efs3.Efs3.from_disk(path, replay=False).check()
        return "journal replayed" in text2, [f"cut after {n} writes: fsck: {e}" for e in errors[:4]], text2

    # first the whole workload, to learn how many writes it makes
    build.make_image(path, "test=powercut:0")
    text = boot(path)
    m = re.search(r"POWERCUT workload finished: (\d+) writes", text)
    if not m:
        os.remove(path)
        return False, text, ["the power-cut workload did not finish"]
    total = int(m.group(1))
    points = sorted({1, 2, *(total * k // 8 for k in range(1, 8)), total - 1})
    replays = 0
    for n in points:
        replayed, errors, text = cut_and_repair(n)
        replays += replayed
        problems += errors
    if not problems and not replays:
        problems.append("no power cut needed the journal: the test did not exercise it")
    os.remove(path)
    summary = f"[vm.py: {total} writes in the workload; cut after {points}; journal replayed {replays} times]"
    return not problems, "\n".join(texts) + "\n" + summary + "\n", problems + ([] if problems else [])


SESSION_TESTS = [("shell", "qemu", shell_test), ("persistence", "qemu", persistence_test),
                 ("powercut", "qemu", powercut_test)]


# ---- VirtualBox ---------------------------------------------------------

def vbox(*args, check=True):
    exe = find("VBoxManage", VBOX_DIRS)
    result = subprocess.run([exe, *args], capture_output=True, text=True)
    if check and result.returncode != 0:
        sys.exit(f"ERROR: VBoxManage {' '.join(args)}\n{result.stderr.strip()}")
    return result.stdout


def vbox_state():
    out = vbox("showvminfo", VM_NAME, "--machinereadable", check=False)
    for line in out.splitlines():
        if line.startswith("VMState="):
            return line.split("=", 1)[1].strip('"')
    return None   # the query itself failed (VBoxSVC busy): unknown


def vbox_running():
    """True unless VirtualBox says, twice in a row, that the VM is off. A single answer is not
    trusted: now and then a query reports the VM as stopped while it runs, and the test then
    powered off a healthy VM in the middle of the boot."""
    for attempt in range(2):
        state = vbox_state()
        if state is None or state not in ("poweroff", "aborted", "saved"):
            return True
        if attempt == 0:
            time.sleep(1)
    return False


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
    vbox("modifyvm", VM_NAME, "--uart1", "0x3F8", "4", "--uart-mode1", "file", serial_log,
         "--rtc-use-utc", "on")   # the RTC holds UTC, as the kernel expects


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
        text = wait_for_done(log, vbox_running)
        if BOOT_OK not in text:
            return text, log
        # type "exit" + Enter into the shell: the VM must power itself off (ACPI)
        deadline = time.time() + 30
        while "opaline:/home$ " not in read_log(log) and time.time() < deadline:
            time.sleep(0.25)
        vbox("controlvm", VM_NAME, "keyboardputstring", "exit", check=False)
        vbox("controlvm", VM_NAME, "keyboardputscancode", "1c", "9c", check=False)   # Enter
        deadline = time.time() + 30
        while vbox_running() and time.time() < deadline:
            time.sleep(0.5)
        text = read_log(log)
        if vbox_running():
            text += "\n[vm.py: the VM did not power off]\n"
        return text, log
    finally:
        if vbox_running():
            vbox("controlvm", VM_NAME, "poweroff", check=False)


# ---- main ---------------------------------------------------------------

def run_tests(target):
    runners = {"qemu": qemu_test, "vbox": vbox_test}
    failed = 0
    # VirtualBox first: it is the more timing-sensitive emulator, so it runs before the QEMU
    # cases write their variant images
    for name, emu, cmdline, expect, forbid in sorted(TESTS, key=lambda t: t[1] != "vbox"):
        if target not in ("all", emu):
            continue
        start = time.time()
        text, log = runners[emu](cmdline)
        missing = [e for e in expect if e not in text]
        present = [f for f in forbid if f in text]
        ok = not missing and not present
        failed += not ok
        print(f"{'TEST_OK' if ok else 'TEST_FAILED'} {emu}/{name}  ({time.time() - start:.1f} s)", flush=True)
        for m in missing:
            print(f"    missing:    {m!r}")
        for p in present:
            print(f"    unexpected: {p!r}")
        if not ok:
            keep = os.path.join(BUILD, f"failed-{emu}-{name}.log")
            shutil.copyfile(log, keep) if os.path.isfile(log) else None
            print(f"    serial log: {os.path.relpath(keep, ROOT)}")
    for name, emu, fn in SESSION_TESTS:
        if target not in ("all", emu):
            continue
        start = time.time()
        ok, text, problems = fn()
        failed += not ok
        print(f"{'TEST_OK' if ok else 'TEST_FAILED'} {emu}/{name}  ({time.time() - start:.1f} s)", flush=True)
        for p in problems:
            print(f"    {p}")
        if not ok:
            keep = os.path.join(BUILD, f"failed-{emu}-{name}.log")
            with open(keep, "w", encoding="utf-8") as f:
                f.write(text)
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
