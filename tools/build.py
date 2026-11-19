# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The OpalineOS authors
"""Builds OpalineOS.

    python tools/build.py           # compile and create build/opaline.img
    python tools/build.py --clean   # delete build/ first

Outputs (in build/):
    BOOTX64.EFI     the UEFI bootloader
    opaline.img     raw GPT disk image (128 MiB) with the EFI System Partition
    opaline.vmdk    VirtualBox descriptor pointing at opaline.img (created once)
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
TOOLS = os.path.join(ROOT, "tools")
IMAGE = os.path.join(BUILD, "opaline.img")
VMDK = os.path.join(BUILD, "opaline.vmdk")

LLVM_DIRS = [r"C:\Program Files\LLVM\bin"]
NASM_DIRS = [r"C:\Program Files\NASM", os.path.join(os.environ.get("LOCALAPPDATA", ""), "bin", "NASM")]

EFI_CFLAGS = [
    "--target=x86_64-unknown-windows", "-std=c11", "-O2",
    "-ffreestanding", "-fno-stack-protector", "-fshort-wchar", "-fno-builtin",
    "-mno-red-zone", "-mno-stack-arg-probe", "-mgeneral-regs-only",
    "-Wall", "-Wextra", "-Werror",
]


def tool(name, dirs):
    """Finds a tool on PATH or in the usual install folders."""
    found = shutil.which(name)
    if found:
        return found
    for d in dirs:
        cand = os.path.join(d, name + ".exe")
        if os.path.isfile(cand):
            return cand
    sys.exit(f"ERROR: '{name}' not found. Install it (see project.md, section 10).")


def run(cmd):
    print("  " + " ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)), flush=True)
    result = subprocess.run(cmd)
    if result.returncode != 0:
        sys.exit(f"ERROR: command failed ({result.returncode})")


def build_bootloader():
    clang = tool("clang", LLVM_DIRS)
    lld_link = tool("lld-link", LLVM_DIRS)
    src_dir = os.path.join(ROOT, "boot", "efi")
    obj_dir = os.path.join(BUILD, "obj", "boot")
    os.makedirs(obj_dir, exist_ok=True)

    objs = []
    for name in sorted(os.listdir(src_dir)):
        if name.endswith(".c"):
            obj = os.path.join(obj_dir, name[:-2] + ".obj")
            run([clang] + EFI_CFLAGS + ["-c", os.path.join(src_dir, name), "-o", obj])
            objs.append(obj)

    out = os.path.join(BUILD, "BOOTX64.EFI")
    run([lld_link, "/nologo", "/subsystem:efi_application", "/entry:efi_main",
         "/nodefaultlib", "/out:" + out] + objs)
    return out


def write_vmdk():
    """A 'monolithicFlat' VMDK lets VirtualBox use the raw image directly.
    Written only once: VirtualBox stores its own UUID in this file."""
    if os.path.isfile(VMDK):
        return
    sectors = os.path.getsize(IMAGE) // 512
    cylinders = min(sectors // (16 * 63), 16383)
    with open(VMDK, "w", newline="\n") as f:
        f.write(f"""# Disk DescriptorFile
version=1
CID=0a11e05e
parentCID=ffffffff
createType="monolithicFlat"

# Extent description
RW {sectors} FLAT "{os.path.basename(IMAGE)}" 0

# The Disk Data Base
#DDB

ddb.virtualHWVersion = "4"
ddb.adapterType="ide"
ddb.geometry.cylinders="{cylinders}"
ddb.geometry.heads="16"
ddb.geometry.sectors="63"
""")


def main():
    ap = argparse.ArgumentParser(description="Build OpalineOS")
    ap.add_argument("--clean", action="store_true", help="delete build/ first (the VirtualBox descriptor is kept)")
    args = ap.parse_args()

    if args.clean and os.path.isdir(BUILD):
        for name in os.listdir(BUILD):
            if name == os.path.basename(VMDK):
                continue
            path = os.path.join(BUILD, name)
            shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
    os.makedirs(BUILD, exist_ok=True)

    print("[1/2] bootloader", flush=True)
    efi = build_bootloader()

    print("[2/2] disk image", flush=True)
    run([sys.executable, os.path.join(TOOLS, "mkimage.py"), IMAGE,
         "--file", "EFI/BOOT/BOOTX64.EFI=" + efi])
    write_vmdk()
    print(f"OK: {IMAGE}")


if __name__ == "__main__":
    main()
