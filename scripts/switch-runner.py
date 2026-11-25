#!/usr/bin/env python3
"""Runs Ambrosia builds on a Switch over the local network, through the Ambrosia runner
(build/switch/ambrosia-runner.nro, started once on the console; BUILD.md, "Test runs from the PC").

The console runs one game at a time, so this script runs one build at a time and waits for it.

    python scripts/switch-runner.py token
        makes the shared token file on this PC if there is none, and says where to copy it
    python scripts/switch-runner.py status
    python scripts/switch-runner.py run build/switch/ambrosia.nro "00_Training --do \\"wait 300; exec DebugOverlay perf-record-for 600f; wait 800; quit\\"" --out scratch/runs/training
        sends the build (skipped when the console has it already), runs it with those options,
        waits for it to exit and saves log.txt and the new FramesNNNN.csv files in --out
    python scripts/switch-runner.py fetch --out scratch/runs/late
        fetches the results the runner holds (after a game that did not come back)

The console's address comes from --console or AMBROSIA_SWITCH (the runner shows it on screen). The
token is read from --token-file, or AMBROSIA_RUNNER_TOKEN_FILE, or ~/.ambrosia/switch-runner-token.txt;
the same text must be in sdmc:/switch/ambrosia/runner/token.txt on the console.

Options are written as in args.txt: separated by spaces, with double quotes around one that has
spaces, but no backslash escapes and no quotes inside an option (the homebrew loader's rules).
Every build must include the game's runner support (BUILD.md); an older one stops on an error.
"""

import argparse
import hashlib
import json
import os
import re
import secrets
import socket
import sys
import time
from pathlib import Path

PORT = 28300
PROTOCOL = 1
DEFAULT_RESULTS = ["sdmc:/switch/ambrosia/log.txt", "sdmc:/switch/ambrosia/user/Profiles/"]
DEFAULT_TOKEN_FILE = Path.home() / ".ambrosia" / "switch-runner-token.txt"
CONSOLE_TOKEN_PATH = "sdmc:/switch/ambrosia/runner/token.txt"
LOG_PATH = "sdmc:/switch/ambrosia/log.txt"
IO_TIMEOUT = 60

class RunnerError(Exception):
    pass


class Unreachable(RunnerError):
    pass


# The connection.

class Connection:
    def __init__(self, address: str, token: str, connect_timeout: float = 5):
        try:
            self.sock = socket.create_connection((address, PORT), timeout=connect_timeout)
        except OSError as error:
            raise Unreachable(f"cannot connect to the runner at {address}:{PORT}: {error}") from None
        self.sock.settimeout(IO_TIMEOUT)
        self.buffer = b""
        self.send_text(f"ambrosia-runner {PROTOCOL}\ntoken {token}\n")

    def close(self):
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def send_text(self, text: str):
        self.sock.sendall(text.encode("utf-8"))

    def _fill(self):
        data = self.sock.recv(256 * 1024)
        if not data:
            raise RunnerError("the runner closed the connection")
        self.buffer += data

    def line(self) -> str:
        while b"\n" not in self.buffer:
            self._fill()
        line, self.buffer = self.buffer.split(b"\n", 1)
        text = line.decode("utf-8", "replace").rstrip("\r")
        if text.startswith("error "):
            raise RunnerError(f"the runner says: {text[6:]}")
        return text

    def read_to(self, path: Path, size: int):
        with open(path, "wb") as out:
            remaining = size
            while remaining > 0:
                if not self.buffer:
                    self._fill()
                chunk = self.buffer[:remaining]
                self.buffer = self.buffer[len(chunk):]
                out.write(chunk)
                remaining -= len(chunk)


def read_token(path: Path) -> str:
    try:
        token = path.read_text(encoding="utf-8").strip()
    except OSError:
        raise RunnerError(f"no token file at {path}; `python scripts/switch-runner.py token` makes one") from None
    if len(token) < 16 or any(c.isspace() for c in token):
        raise RunnerError(f"the token in {path} must be 16 or more characters with no spaces")
    return token


def status(address: str, token: str) -> str:
    with Connection(address, token) as conn:
        conn.send_text("status\n")
        reply = conn.line()
    if not reply.startswith("state "):
        raise RunnerError(f"unexpected reply: {reply}")
    return reply


# One job.

def check_options(options: str):
    if "\n" in options or "\r" in options:
        raise RunnerError("options cannot have line breaks")
    if "\\" in options:
        raise RunnerError("options cannot have backslashes (the homebrew loader has no escapes)")
    if options.count('"') % 2:
        raise RunnerError("options have an unmatched double quote")


def send_job(address: str, token: str, nro: str, options: str, results: list[str]) -> str:
    """Sends the job; returns the session of the runner, which ends as it starts the game."""
    check_options(options)
    lines = ["job"]
    data_path = None
    if nro.startswith("sdmc:/"):
        lines.append(f"nro-path {nro}")
    else:
        data_path = Path(nro)
        if not data_path.is_file():
            raise RunnerError(f"no such build: {nro}")
        digest = hashlib.sha256(data_path.read_bytes()).hexdigest()
        lines += [f"nro-sha256 {digest}", f"nro-size {data_path.stat().st_size}"]
    lines.append(f"args {options}")
    lines += [f"result {r}" for r in results]
    lines.append("end")
    with Connection(address, token) as conn:
        conn.send_text("\n".join(lines) + "\n")
        reply = conn.line()
        if reply == "send-nro":
            started = time.monotonic()
            size = data_path.stat().st_size
            with open(data_path, "rb") as source:
                while chunk := source.read(1 << 20):
                    conn.sock.sendall(chunk)
            print(f"  sent {data_path.name}, {size / 1048576:.1f} MB in {time.monotonic() - started:.1f} s", flush=True)
            reply = conn.line()
        elif reply == "have-nro":
            print(f"  the console has {data_path.name} already", flush=True)
            reply = conn.line()
        if not reply.startswith("launching "):
            raise RunnerError(f"unexpected reply: {reply}")
        return reply.split()[1]


def parse_status(reply: str) -> tuple[str, str]:
    """The state (idle or results) and session of a status reply."""
    words = reply.split()
    if len(words) < 4 or words[0] != "state" or words[2] != "session":
        raise RunnerError(f"unexpected reply: {reply}")
    return words[1], words[3]


def wait_for_runner(address: str, token: str, timeout: float, launcher_session: str) -> None:
    """Waits until a runner other than the one that started the game answers with results."""
    time.sleep(3)
    deadline = time.monotonic() + timeout
    last_note = time.monotonic()
    while True:
        try:
            state, session = parse_status(status(address, token))
            if session != launcher_session:
                if state == "results":
                    return
                raise RunnerError("the runner came back without results; the game may not have started")
        except Unreachable:
            pass
        now = time.monotonic()
        if now > deadline:
            raise Unreachable(
                f"the runner has not come back {timeout:.0f} s after starting the game. The game has hung, "
                "or it crashed (a crash closes the runner as well). Someone has to look at the console: if "
                "Ambrosia is still on screen, close it (HOME, then X to close the software), then start the runner "
                "again (hold R while starting a game, choose Ambrosia runner). Then `switch-runner.py fetch` gets "
                "the log of the run that failed.")
        if now - last_note > 60:
            print(f"  still running ({now - deadline + timeout:.0f} s)", flush=True)
            last_note = now
        time.sleep(3)


def fetch_results(address: str, token: str, out: Path) -> dict:
    """Saves the runner's result files in out; returns what came back."""
    out.mkdir(parents=True, exist_ok=True)
    got = {"returned": False, "files": [], "missing": []}
    with Connection(address, token) as conn:
        conn.send_text("results\n")
        if conn.line() != "results":
            raise RunnerError("unexpected reply to results")
        while True:
            line = conn.line()
            if line == "end":
                break
            if line.startswith("returned "):
                got["returned"] = line.split()[1] == "yes"
            elif line.startswith("console "):
                got["console"] = line[8:]
                (out / "console.txt").write_text(line[8:] + "\n", encoding="utf-8")
            elif line.startswith("missing "):
                got["missing"].append(line[8:])
            elif line.startswith("file "):
                path, size = line[5:].rsplit(" ", 1)
                local = out / path.rsplit("/", 1)[-1]
                conn.read_to(local, int(size))
                got["files"].append(local)
            else:
                raise RunnerError(f"unexpected line in results: {line}")
        conn.send_text("ack\n")
        conn.line()
    return got


def fetch_crash_report(address: str, token: str, out: Path) -> Path | None:
    """Saves the newest of Atmosphère's crash reports in out, if the runner has one to send."""
    with Connection(address, token) as conn:
        conn.send_text("crash-report\n")
        line = conn.line()
        if line == "none":
            conn.line()
            return None
        if not line.startswith("file "):
            raise RunnerError(f"unexpected reply to crash-report: {line}")
        path, size, _mtime = line[5:].rsplit(" ", 2)
        out.mkdir(parents=True, exist_ok=True)
        local = out / ("crash-report-" + path.rsplit("/", 1)[-1])
        conn.read_to(local, int(size))
        conn.line()
    print(f"  newest crash report on the card: {path} -> {local}", flush=True)
    return local


def describe_log(log: Path) -> dict:
    """What the game's log says about the run: how it ended, the GL driver, when the first frame
    came, and the worldhash lines (frame -> hash)."""
    info = {"fatal": None, "crash": None, "worldhash": None, "hashes": {}, "gl": None, "first frame s": None, "first frame": False}
    if not log.is_file():
        return info
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("Critical: crash: "):
            info["crash"] = line[17:]
        elif line.startswith("Critical: ") and not info["fatal"]:
            info["fatal"] = line[10:]
        elif line.startswith("Init: OpenGL ") and not info["gl"]:
            info["gl"] = line[6:]
        elif line.startswith("Init: first frame drawn"):
            info["first frame"] = True
            if m := re.search(r"([0-9.]+) s after start", line):
                info["first frame s"] = float(m.group(1))
        elif "worldhash" in line and "hash=" in line and "worldhash-dump" not in line:
            value = line.split(" hash=", 1)[1].split()[0] if " hash=" in line else line.split("hash=", 1)[1].split()[0]
            info["worldhash"] = value
            if m := re.search(r"frame=(\d+)", line):
                info["hashes"][int(m.group(1))] = value
    return info


def nro_digest(nro: str) -> str | None:
    return hashlib.sha256(Path(nro).read_bytes()).hexdigest() if not nro.startswith("sdmc:/") else None


def run_job(address: str, token: str, nro: str, options: str, out: Path, results: list[str], timeout: float) -> dict:
    """Runs one job and saves its files, console.txt and result.json (what ran and how it went)
    in out. On a fatal error or crash it also fetches the newest crash report."""
    print(f"run {nro}: {options}", flush=True)
    if LOG_PATH not in results:
        results = [LOG_PATH] + list(results)
    started = time.monotonic()
    digest = nro_digest(nro)
    record = {"nro": str(nro), "nro sha256": digest, "options": options, "started": time.strftime("%Y-%m-%d %H:%M:%S")}
    out.mkdir(parents=True, exist_ok=True)

    def save(state: dict):
        record.update({k: v for k, v in state.items() if k != "files"})
        record["files"] = [str(f.name) for f in state.get("files", [])]
        (out / "result.json").write_text(json.dumps(record, indent=1, default=str) + "\n", encoding="utf-8")

    session = send_job(address, token, nro, options, results)
    try:
        wait_for_runner(address, token, timeout, session)
    except Unreachable:
        save({"outcome": f"no answer from the runner {timeout:.0f} s after the start (hang or crash)", "seconds": time.monotonic() - started})
        raise
    got = fetch_results(address, token, out)
    got.update(describe_log(out / "log.txt"))
    got["seconds"] = time.monotonic() - started
    if got["fatal"] or got["crash"] or not got["returned"]:
        got["crash report"] = fetch_crash_report(address, token, out)
    got["outcome"] = "returned" if got["returned"] and not got["fatal"] and not got["crash"] else "failed"
    save(got)
    state = "returned" if got["returned"] else "did NOT return to the runner"
    note = f"fatal: {got['fatal']}" if got["fatal"] else f"crash: {got['crash']}" if got["crash"] else "no error in the log"
    print(f"  {state} after {got['seconds']:.0f} s, {note}; {len(got['files'])} files in {out}", flush=True)
    print(f"  console: {got.get('console', 'not reported')}; {got.get('gl') or 'no GL line'}", flush=True)
    return got


# Commands.

def command_token(args):
    path = args.token_file
    if path.exists():
        print(f"{path} exists already.")
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(secrets.token_hex(24) + "\n", encoding="utf-8")
        print(f"Made {path}.")
    print(f"Copy it to the SD card as {CONSOLE_TOKEN_PATH} (the same text, one line).")
    return 0


def command_status(args, token):
    print(status(args.console, token))
    return 0


def command_run(args, token):
    got = run_job(args.console, token, args.nro, args.options, Path(args.out), args.result or DEFAULT_RESULTS, args.timeout)
    return 0 if got["returned"] and not got["fatal"] else 1


def command_fetch(args, token):
    out = Path(args.out)
    got = fetch_results(args.console, token, out)
    got.update(describe_log(out / "log.txt"))
    print(f"returned to the runner: {'yes' if got['returned'] else 'no'}; fatal: {got['fatal']}; crash: {got['crash']}")
    for f in got["files"]:
        print(f"  {f}")
    fetch_crash_report(args.console, token, out)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--console", default=os.environ.get("AMBROSIA_SWITCH"), help="the console's address (AMBROSIA_SWITCH)")
    parser.add_argument("--token-file", type=Path, default=Path(os.environ.get("AMBROSIA_RUNNER_TOKEN_FILE", DEFAULT_TOKEN_FILE)))
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("token")
    sub.add_parser("status")
    run = sub.add_parser("run")
    run.add_argument("nro", help="a local .nro, or the sdmc:/switch/ambrosia/... path of one on the card")
    run.add_argument("options", help="the game's options, as in args.txt")
    run.add_argument("--out", required=True)
    run.add_argument("--timeout", type=float, default=900, help="seconds to wait for the game to return to the runner (default 900)")
    run.add_argument("--result", action="append", help="a file or folder (ending in /) on the card to fetch; repeat for more (default: log.txt and new files in user/Profiles/; log.txt always)")
    fetch = sub.add_parser("fetch")
    fetch.add_argument("--out", required=True)
    args = parser.parse_args()

    try:
        if args.command == "token":
            return command_token(args)
        if not args.console:
            parser.error("the console's address is needed: --console or AMBROSIA_SWITCH")
        token = read_token(args.token_file)
        command = {"status": command_status, "run": command_run, "fetch": command_fetch}[args.command]
        return command(args, token)
    except RunnerError as error:
        print(f"switch-runner: {error}", file=sys.stderr)
        return 3 if isinstance(error, Unreachable) else 1


if __name__ == "__main__":
    sys.exit(main())