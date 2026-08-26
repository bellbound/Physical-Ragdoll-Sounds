#!/usr/bin/env python
"""Edit the config of a testbench that is already running.

    python tools/tune.py status
    python tools/tune.py list
    python tools/tune.py get slide
    python tools/tune.py set Slide:fSlideMinDurationMs=120 -m "slides were starting too eagerly"
    python tools/tune.py load config_24_08_7

`set` patches the config the focused side is playing, saves the result as a new
file in `testbench/configs/`, and selects it there - no restart, no reload, and
the change is heard on the next play. A connected game gets it on the same
frame, because the testbench already pushes whatever the focused side holds.

The new file is always a *new* name (the next number in the family it was
patched from), never over the one it came from, and the edit lands on the
testbench's undo stack - so Ctrl+Z in the app takes back anything this does.

It talks to the control socket described in `testbench/src/Control.h`: loopback
TCP on the devbench port plus one, one connection per command. A refused
connection means the testbench is not running - start it and try again.
"""

from __future__ import annotations

import argparse
import re
import socket
import struct
import sys
from pathlib import Path

MAGIC = 0x21534452  # "RDS!"
MSG_REQUEST = 200
MSG_REPLY = 201
HEADER = struct.Struct("<IHHI")
MAX_PAYLOAD = 8 * 1024 * 1024

DEFAULT_DEVBENCH_PORT = 27860

REPO = Path(__file__).resolve().parent.parent
# The same file the testbench reads its port out of. See App::StartLink.
GENERAL_INI = (
    REPO.parent.parent
    / "papyrus"
    / "mods"
    / "Physical Ragdoll Sounds"
    / "deployment_files"
    / "main"
    / "SKSE"
    / "Plugins"
    / "RagdollSounds"
    / "RagdollSounds.ini"
)


def control_port(override: int | None = None) -> int:
    """The port the running testbench is listening on for us.

    Read out of the deployed RagdollSounds.ini rather than hardcoded, so a
    machine that had to move the devbench port off 27860 moves this one with
    it - the testbench derives its control port the same way.
    """
    if override:
        return override
    port = DEFAULT_DEVBENCH_PORT
    try:
        text = GENERAL_INI.read_text(encoding="utf-8", errors="replace")
        found = re.search(r"^\s*iDevbenchPort\s*=\s*(\d+)", text, re.M)
        if found:
            port = int(found.group(1))
    except OSError:
        pass
    return port + 1


def talk(request: str, port: int, timeout: float = 15.0) -> list[tuple[str, str]]:
    """One request, one reply, hang up. Returns the reply's key/value lines."""
    payload = request.encode("utf-8")
    frame = HEADER.pack(MAGIC, MSG_REQUEST, 0, len(payload)) + payload
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(frame)
        head = recv_exactly(sock, HEADER.size)
        magic, kind, _flags, size = HEADER.unpack(head)
        if magic != MAGIC or size > MAX_PAYLOAD:
            raise RuntimeError("that is not the testbench's control socket")
        if kind != MSG_REPLY:
            raise RuntimeError(f"unexpected reply type {kind}")
        body = recv_exactly(sock, size).decode("utf-8", errors="replace")
    out: list[tuple[str, str]] = []
    for line in body.splitlines():
        if not line.strip():
            continue
        key, _, value = line.partition("=")
        out.append((key.strip(), value))
    return out


def recv_exactly(sock: socket.socket, count: int) -> bytes:
    chunks: list[bytes] = []
    got = 0
    while got < count:
        chunk = sock.recv(count - got)
        if not chunk:
            raise RuntimeError("the testbench closed the connection")
        chunks.append(chunk)
        got += len(chunk)
    return b"".join(chunks)


def value_of(reply: list[tuple[str, str]], key: str, default: str = "") -> str:
    for name, value in reply:
        if name == key:
            return value
    return default


def all_of(reply: list[tuple[str, str]], key: str) -> list[str]:
    return [value for name, value in reply if name == key]


def build_request(args: argparse.Namespace) -> str:
    lines = [f"op={args.op}"]
    if getattr(args, "note", None):
        lines.append(f"note={args.note}")
    if getattr(args, "base", None):
        lines.append(f"base={args.base}")
    if getattr(args, "name", None):
        lines.append(f"name={args.name}")
    if getattr(args, "filter", None):
        lines.append(f"filter={args.filter}")
    if getattr(args, "side", None):
        lines.append(f"side={args.side}")
    if getattr(args, "no_save", False):
        lines.append("save=0")
    for assignment in getattr(args, "assignments", []) or []:
        lines.append(f"set={assignment}")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Patch the config of a running RagdollSoundsTestbench.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--port", type=int, default=0, help="control port (default: devbench + 1)")
    sub = parser.add_subparsers(dest="op", required=True)

    p = sub.add_parser("status", help="is it running, and what is it playing")
    p = sub.add_parser("list", help="the configs in the picker, newest first")

    p = sub.add_parser("get", help="the parameters this side is holding")
    p.add_argument("filter", nargs="?", default="", help="only keys or groups containing this")

    p = sub.add_parser("set", help="patch, save as a new config, and select it")
    p.add_argument("assignments", nargs="+", metavar="Section:Key=value")
    p.add_argument("-m", "--note", default="", help="why - goes into the file and the log")
    p.add_argument("--from", dest="base", default="", help="patch this named config instead of "
                                                           "whatever the side is holding")
    p.add_argument("--name", default="", help="stem for the new file (default: the next number "
                                              "in the family it came from)")
    p.add_argument("--side", choices=("A", "B"), default="", help="default: whichever has focus")
    p.add_argument("--no-save", action="store_true",
                   help="patch in place without writing a file - an audition, not a config")

    p = sub.add_parser("load", help="select a config that already exists")
    p.add_argument("name", help="its stem, or a substring that picks out exactly one")
    p.add_argument("--side", choices=("A", "B"), default="")

    args = parser.parse_args()
    port = control_port(args.port)

    try:
        reply = talk(build_request(args), port)
    except (ConnectionRefusedError, OSError) as error:
        print(f"no testbench on 127.0.0.1:{port} ({error}).", file=sys.stderr)
        print("Start RagdollSoundsTestbench.exe - this edits a running session, it cannot "
              "start one.", file=sys.stderr)
        return 2
    except RuntimeError as error:
        print(str(error), file=sys.stderr)
        return 2

    if value_of(reply, "ok") != "1":
        print(value_of(reply, "error", "the testbench refused it"), file=sys.stderr)
        return 1

    if args.op in ("status", "ping"):
        print(f"testbench up on 127.0.0.1:{port}")
        print(f"  side {value_of(reply, 'side')} playing {value_of(reply, 'config')}"
              f"{' *' if value_of(reply, 'unsaved') == '1' else ''}"
              f"   ({value_of(reply, 'configs')} configs)")
        if value_of(reply, "note"):
            print(f"  last patch: {value_of(reply, 'note')}")
        print(f"  take {value_of(reply, 'take') or '(none)'}")
        print(f"  game {value_of(reply, 'game')}, pushing config: "
              f"{'yes' if value_of(reply, 'push') == '1' else 'no'}")
    elif args.op == "list":
        selected = value_of(reply, "selected")
        for row in all_of(reply, "config"):
            name = row.split("\t")[0]
            print(f"{'>' if name == selected else ' '} {name}")
    elif args.op == "get":
        print(f"# {value_of(reply, 'config')}")
        for row in all_of(reply, "param"):
            print(row)
    elif args.op == "load":
        print(f"loaded {value_of(reply, 'name')}")
    else:  # set
        for row in all_of(reply, "applied"):
            print(f"  {row}")
        if value_of(reply, "saved") == "1":
            print(f"saved and selected: {value_of(reply, 'name')} "
                  f"(side {value_of(reply, 'side')})")
        else:
            print(f"patched {value_of(reply, 'name')} in place - not written to a file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
