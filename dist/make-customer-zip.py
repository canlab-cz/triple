#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
"""Customer bundle: one zip with all packages, a start page and the guides as HTML.

    python dist/make-customer-zip.py            (after dist/build-all.ps1)

Writes dist/out/USB2CAN-Triple-<version>.zip:

    USB2CAN-Triple-<version>/
        START-HERE.html, README.txt     which file for which system, installation, first steps
        Windows/                        setup.exe and portable zip
        Linux/Debian-Ubuntu/            the three .deb packages
        Linux/Other-Linux/              tarball with install.sh
        Documentation/                  user guides as HTML with screenshots
        SHA256SUMS, LICENSES.txt

Needs the Python package "markdown" (pip install markdown).
"""

import hashlib
import html
import os
import re
import shutil
import sys
import zipfile

import markdown

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "dist", "out")
VERSION = open(os.path.join(REPO, "dist", "VERSION")).read().strip()
NAME = "USB2CAN-Triple-%s" % VERSION
GITHUB = "https://github.com/canlab-cz/triple/blob/master/"

PACKAGES = {
    "Windows": ["usb2can-triple-%s-windows-x64-setup.exe", "usb2can-triple-%s-windows-x64.zip"],
    "Linux/Debian-Ubuntu": ["usb2cansocketcan-dkms_%s-1_all.deb", "usb2can-triple-tools_%s-1_amd64.deb",
                            "savvycan-triple_%s-1_amd64.deb"],
    "Linux/Other-Linux": ["usb2can-triple-%s-linux-x86_64.tar.gz"],
}

# guide -> title; these become Documentation/<name>.html
GUIDES = [
    ("savvycan-guide", "SavvyCAN with the USB2CAN Triple: user guide"),
    ("reverse-engineering", "Reverse engineering CAN traffic"),
    ("linux-driver", "Linux driver (SocketCAN)"),
    ("protocol", "USB2CAN Triple host protocol"),
]

CSS = """
body { font-family: Segoe UI, Helvetica, Arial, sans-serif; line-height: 1.5; color: #1f2328; background: #fff;
       max-width: 980px; margin: 0 auto; padding: 24px 16px 64px; }
h1, h2, h3 { line-height: 1.25; } h1 { border-bottom: 1px solid #d0d7de; padding-bottom: .3em; }
h2 { border-bottom: 1px solid #d0d7de; padding-bottom: .3em; margin-top: 2em; }
a { color: #0969da; text-decoration: none; } a:hover { text-decoration: underline; }
code { font-family: Consolas, Menlo, monospace; font-size: 90%; background: #f6f8fa; padding: .1em .3em; border-radius: 4px; }
pre { background: #f6f8fa; padding: 12px 16px; overflow-x: auto; border-radius: 6px; }
pre code { background: none; padding: 0; }
table { border-collapse: collapse; margin: 1em 0; display: block; overflow-x: auto; }
th, td { border: 1px solid #d0d7de; padding: 6px 12px; vertical-align: top; } th { background: #f6f8fa; }
img { max-width: 100%; border: 1px solid #d0d7de; border-radius: 6px; }
blockquote { margin: 0; padding: 0 1em; color: #59636e; border-left: .25em solid #d0d7de; }
.nav { font-size: 90%; margin-bottom: 1em; } .box { background: #f6f8fa; border: 1px solid #d0d7de;
       border-radius: 6px; padding: 8px 16px; }
"""


def page(title, body, nav=True):
    n = '<div class="nav"><a href="%s">&larr; Start page</a></div>' % ("../START-HERE.html" if nav else "") if nav else ""
    return ('<!DOCTYPE html>\n<html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width, initial-scale=1">'
            '<title>%s</title><style>%s</style></head><body>%s\n%s\n</body></html>\n'
            % (html.escape(title), CSS, n, body))


def fix_links(text, guide_names):
    """Markdown links of the repository -> links inside the bundle (or GitHub)."""
    def repl(m):
        label, target = m.group(1), m.group(2)
        if re.match(r"^(https?:|mailto:|#)", target):
            return m.group(0)
        path, _, anchor = target.partition("#")
        base = os.path.basename(path)
        stem = base[:-3] if base.endswith(".md") else None
        if stem in guide_names:
            new = stem + ".html" + ("#" + anchor if anchor else "")
        elif path.endswith("dist/README.md"):
            new = "../START-HERE.html"           # the packages: this bundle's start page
        elif path.startswith("img/"):
            new = target
        else:
            # files of the repository: link to the project on GitHub
            rel = os.path.normpath(os.path.join("doc", path)).replace("\\", "/")
            new = GITHUB + rel + ("#" + anchor if anchor else "")
        return "[%s](%s)" % (label, new)
    return re.sub(r"\[([^\]]*)\]\(([^)\s]+)\)", repl, text)


def render(md_text):
    return markdown.markdown(md_text, extensions=["tables", "fenced_code", "toc", "sane_lists"],
                             extension_configs={"toc": {"slugify": lambda v, sep: re.sub(r"[^\w\- ]", "", v).strip().lower().replace(" ", sep)}},
                             tab_length=2)


START = """# USB2CAN Triple software {v}

Software for the CANLAB **USB2CAN Triple** adapter (CAN1 and CAN2 classic CAN,
CAN3 CAN FD):

* **SavvyCAN**, a CAN bus analyzer, with a native USB2CAN Triple connection and
  tools: Bus Statistics, CANopen (SDO, PDOs from EDS, block transfer, LSS),
  Data Panel, Response Meter, CAN Calculator, PP2CAN log files, printing, and
  for reverse engineering a Signal Finder and an AI Assistant.
* A **Linux driver** that makes the adapter three SocketCAN interfaces.
* **Command line tools**.

## Which file do I need?

| System | File | |
|---|---|---|
| Windows 10 / 11 (64 bit) | `Windows/usb2can-triple-{v}-windows-x64-setup.exe` | Installer: SavvyCAN, tools, documentation, Start menu entries |
| Windows, without installing | `Windows/usb2can-triple-{v}-windows-x64.zip` | Unzip anywhere, run `SavvyCAN\\SavvyCAN.exe` |
| Ubuntu 22.04 or later, Debian 12 or later | `Linux/Debian-Ubuntu/*.deb` | Driver (DKMS), SavvyCAN, tools |
| Other Linux (x86-64; e.g. Fedora, openSUSE, Arch) | `Linux/Other-Linux/usb2can-triple-{v}-linux-x86_64.tar.gz` | Driver source with installer, SavvyCAN with its libraries, tools |

## Windows

1. Run `usb2can-triple-{v}-windows-x64-setup.exe` and follow the installer.
   No driver is needed: the adapter uses Windows' built-in USB serial driver
   and appears as a COM port.
2. Start **SavvyCAN (USB2CAN Triple)** from the Start menu.
3. Connection > Open Connection Window > Add New Device Connection >
   **CANLAB USB2CAN Triple (serial)**, choose the adapter's COM port,
   Create New Connection. Set the bitrate of each bus under Bus Details and
   press Save Bus Settings.

## Ubuntu, Debian

```
sudo apt install ./usb2cansocketcan-dkms_{v}-1_all.deb \\
                 ./usb2can-triple-tools_{v}-1_amd64.deb ./savvycan-triple_{v}-1_amd64.deb
```

Run it in the `Linux/Debian-Ubuntu` folder. The kernel headers must be
installed (`linux-headers-generic` on Ubuntu, `linux-headers-amd64` on Debian)
so that the driver can be built. Plugging in the adapter then creates the
interfaces `can0` (CAN1), `can1` (CAN2) and `can2` (CAN3, CAN FD). Set their
bitrates and bring them up, for example:

```
sudo ip link set can0 up type can bitrate 250000
sudo ip link set can1 up type can bitrate 500000
sudo ip link set can2 up type can bitrate 125000 dbitrate 6700000 fd on
candump any
```

All supported bitrates are in the Linux driver guide.

Start the analyzer with `savvycan-triple` or from the applications menu. In
SavvyCAN, use the USB2CAN Triple connection (as on Windows, port `ttyACM0`)
or, with the driver attached, the SocketCAN interfaces `can0` .. `can2`.

## Other Linux distributions

```
tar xzf usb2can-triple-{v}-linux-x86_64.tar.gz
cd usb2can-triple-{v}-linux-x86_64
sudo ./install.sh
```

This installs the driver (with DKMS if available, otherwise for the running
kernel; needs make, gcc and the kernel headers), the automatic setup when the
adapter is plugged in, and SavvyCAN and the tools in `/opt/usb2can-triple`.
`sudo ./install.sh --no-driver` skips the driver, `sudo ./install.sh --uninstall`
removes everything.

## Documentation

* [SavvyCAN user guide](Documentation/savvycan-guide.html): connection,
  capture, sending, Bus Statistics, CANopen, printing, troubleshooting
* [Reverse engineering guide](Documentation/reverse-engineering.html): Signal
  Finder, comparing time windows, multiplexed messages, DBC export, the AI
  Assistant with examples
* [Linux driver](Documentation/linux-driver.html): bitrates, CAN FD timing,
  bus state and statistics, automatic setup, troubleshooting
* [Protocol](Documentation/protocol.html): the adapter's USB protocol

The reverse engineering guide works with a simulated capture you can create
yourself: `python tools/re_demo.py demo.log` (Windows: in the installation
folder) or `re-demo demo.log` (Debian, Ubuntu); load `demo.log` in SavvyCAN.

The **AI Assistant** in SavvyCAN uses Claude through your own Anthropic API
key. Your questions and analysis results from the capture are sent to
Anthropic; you set limits on tokens, requests and bus access. See the
reverse engineering guide before using it.

## Checking the download

`SHA256SUMS` lists the SHA-256 checksum of every file. Windows:
`certutil -hashfile <file> SHA256`; Linux: `sha256sum -c SHA256SUMS`.

## Licenses

See `LICENSES.txt`. The software is free and open source; source code:
https://github.com/canlab-cz/triple

CANLAB s.r.o., https://www.canlab.cz, info@canlab.cz
"""

LICENSES = """USB2CAN Triple software {v}: licenses
=====================================

Linux kernel module (usb2cansocketcan)      GNU GPL version 2
Protocol codec, SavvyCAN additions, tools,
scripts, documentation                      MIT
SavvyCAN (github.com/collin80/SavvyCAN)     MIT
Qt (in the Windows packages and in the
Linux tarball)                              GNU LGPL version 3
MinGW-w64 runtime (Windows)                 GPL 3 with the GCC Runtime Library
                                            Exception; winpthreads: MIT-style

The full license texts are inside each package: Windows "licenses" folder,
Linux tarball "LICENSES" and "savvycan" folders, Debian packages
/usr/share/doc/<package>/copyright and /usr/share/common-licenses.

Source code: https://github.com/canlab-cz/triple
SavvyCAN: https://github.com/collin80/SavvyCAN
Qt: https://download.qt.io
"""


def main():
    stage_root = os.path.join(REPO, "dist", "work", "customer")
    stage = os.path.join(stage_root, NAME)
    if os.path.isdir(stage_root):
        shutil.rmtree(stage_root)
    os.makedirs(stage)

    missing = []
    for folder, names in PACKAGES.items():
        os.makedirs(os.path.join(stage, folder))
        for n in names:
            src = os.path.join(OUT, n % VERSION)
            if not os.path.isfile(src):
                missing.append(src)
                continue
            shutil.copy2(src, os.path.join(stage, folder))
    if missing:
        sys.exit("missing packages (run dist/build-all.ps1 first):\n  " + "\n  ".join(missing))

    docdir = os.path.join(stage, "Documentation")
    os.makedirs(os.path.join(docdir, "img"))
    names = {g for g, _ in GUIDES}
    used_imgs = set()
    for g, title in GUIDES:
        text = open(os.path.join(REPO, "doc", g + ".md"), encoding="utf-8").read()
        used_imgs.update(re.findall(r"\(img/([^)\s]+)\)", text))
        body = render(fix_links(text, names))
        open(os.path.join(docdir, g + ".html"), "w", encoding="utf-8").write(page(title, body))
    for img in sorted(used_imgs):
        shutil.copy2(os.path.join(REPO, "doc", "img", img), os.path.join(docdir, "img", img))

    start_md = START.format(v=VERSION)
    open(os.path.join(stage, "START-HERE.html"), "w", encoding="utf-8").write(
        page("USB2CAN Triple software %s" % VERSION, render(start_md), nav=False))
    plain = re.sub(r"\[([^\]]*)\]\(([^)]+)\)", r"\1 (\2)", start_md).replace("**", "").replace("`", "")
    open(os.path.join(stage, "README.txt"), "w", encoding="utf-8", newline="\r\n").write(plain)
    open(os.path.join(stage, "LICENSES.txt"), "w", encoding="utf-8", newline="\r\n").write(LICENSES.format(v=VERSION))

    sums = []
    for folder, names_ in PACKAGES.items():
        for n in names_:
            p = os.path.join(stage, folder, n % VERSION)
            sums.append("%s  %s/%s" % (hashlib.sha256(open(p, "rb").read()).hexdigest(), folder, n % VERSION))
    open(os.path.join(stage, "SHA256SUMS"), "w", newline="\n").write("\n".join(sums) + "\n")

    zpath = os.path.join(OUT, NAME + ".zip")
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for root, _, files in os.walk(stage):
            for f in sorted(files):
                full = os.path.join(root, f)
                arc = os.path.relpath(full, stage_root).replace("\\", "/")
                info = zipfile.ZipInfo.from_file(full, arc)
                if f.endswith((".sh",)):
                    info.external_attr = 0o100755 << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(full, "rb") as fh:
                    z.writestr(info, fh.read(), compresslevel=9)
    print("%s  %.1f MB" % (zpath, os.path.getsize(zpath) / 1e6))
    for root, _, files in os.walk(stage):
        for f in sorted(files):
            print("  " + os.path.relpath(os.path.join(root, f), stage_root).replace("\\", "/"))


if __name__ == "__main__":
    main()
