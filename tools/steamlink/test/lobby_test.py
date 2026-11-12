#!/usr/bin/env python3
"""OOTSteamLink tests for the game's Co-op Lobby (--status-file mode) over the mock Steam library.

Same folder layout as stream_test.py: <work>/H (host side) and <work>/J (join side), each with OOTSteamLink(.exe)
and the mock library. The host relays to a local echo server instead of OOTServer (--no-server).
Usage: python lobby_test.py --work <dir with H and J>

Checks:
  1 host: the status file says "steam <name>" and "ready host <port> <lobby>"
  2 join without a lobby id: the status file lists the hosting friend ("friends 1", "friend <lobby> Host")
  3 the game picks the friend with <status>.pick: "ready join <port> Host", and bytes round-trip through the relay
  4 the host reports the friend ("peer+ Client")
  5 <status>.stop stops the join side ("stopped"), the host reports "peer- Client"
  6 <status>.stop stops the host ("stopped")
  7 --parent-pid: the tool stops by itself when that process is gone
  8 an error is written to the status file and the tool exits without waiting for a key press
"""
import argparse
import os
import socket
import subprocess
import sys
import threading
import time

WIN = os.name == "nt"
EXE = "OOTSteamLink.exe" if WIN else "OOTSteamLink"

ECHO_PORT = 22160
MOCK_BASE = "48500"

results = []


def report(name, ok, detail=""):
    results.append(ok)
    print("%s  %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def echo_server(port):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)

    def handle(c):
        try:
            while True:
                d = c.recv(65536)
                if not d:
                    break
                c.sendall(d)
        except OSError:
            pass
        finally:
            c.close()

    def run():
        while True:
            try:
                c, _ = srv.accept()
            except OSError:
                return
            threading.Thread(target=handle, args=(c,), daemon=True).start()

    threading.Thread(target=run, daemon=True).start()
    return srv


def spawn(folder, name, args):
    env = dict(os.environ, STEAMLINK_MOCK_NAME=name, STEAMLINK_MOCK_PORT_BASE=MOCK_BASE)
    flags = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if WIN else {"start_new_session": True}
    return subprocess.Popen([os.path.join(folder, EXE)] + args, cwd=folder, env=env, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **flags)


def status_path(folder):
    path = os.path.join(folder, "lobby-status.txt")
    for suffix in ("", ".pick", ".stop"):
        try:
            os.remove(path + suffix)
        except OSError:
            pass
    return path


def lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return [l.rstrip("\n") for l in f]
    except OSError:
        return []


def wait_for(path, prefix, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        for l in lines(path):
            if l.startswith(prefix):
                return l
        time.sleep(0.1)
    return None


def touch(path, text=""):
    with open(path, "w") as f:
        f.write(text)


def wait_exit(p, timeout=15):
    try:
        p.wait(timeout)
        return True
    except subprocess.TimeoutExpired:
        p.kill()
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    hdir, jdir = os.path.join(work, "H"), os.path.join(work, "J")
    srv = echo_server(ECHO_PORT)

    hs = status_path(hdir)
    host = spawn(hdir, "Host", ["host", "--no-server", "--no-game", "--port", str(ECHO_PORT), "--status-file", hs])
    js = status_path(jdir)
    join = None
    try:
        steam = wait_for(hs, "steam ")
        ready = wait_for(hs, "ready host ")
        report("1 host ready", bool(steam and ready), "(%s / %s)" % (steam, ready))
        lobby = ready.split()[3] if ready else "0"

        join = spawn(jdir, "Client", ["join", "--no-launch", "--status-file", js])
        friend = wait_for(js, "friend ")
        report("2 hosting friend listed", bool(wait_for(js, "friends 1") and friend and friend.split()[1] == lobby
                                               and friend.endswith(" Host")), "(%s)" % friend)

        touch(js + ".pick", lobby)
        jready = wait_for(js, "ready join ")
        ok = False
        if jready and jready.endswith(" Host"):
            port = int(jready.split()[2])
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            s.settimeout(10)
            payload = os.urandom(50000)
            s.sendall(payload)
            got = b""
            while len(got) < len(payload):
                d = s.recv(65536)
                if not d:
                    break
                got += d
            s.close()
            ok = got == payload
        report("3 picked friend, relay works", ok, "(%s)" % jready)
        report("4 host sees the friend", bool(wait_for(hs, "peer+ Client")))

        touch(js + ".stop")
        stopped = wait_exit(join) and wait_for(js, "stopped", 2)
        report("5 join stops on request", bool(stopped and wait_for(hs, "peer- Client", 25)))

        touch(hs + ".stop")
        report("6 host stops on request", bool(wait_exit(host) and wait_for(hs, "stopped", 2)))

        # 7: a short-lived "game" process; the join side waits for a pick and must stop when it is gone
        game = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(2)"])
        js = status_path(jdir)
        join = spawn(jdir, "Client", ["join", "--no-launch", "--status-file", js, "--parent-pid", str(game.pid)])
        game.wait()
        report("7 stops when the game is gone", wait_exit(join, 10) and bool(wait_for(js, "stopped", 2)))

        # 8: hosting on a port that is taken (the echo server) fails with an error line
        hs = status_path(hdir)
        host = spawn(hdir, "Host", ["host", "--port", str(ECHO_PORT), "--server-exe", sys.executable,
                                    "--status-file", hs])
        exited = wait_exit(host, 10)
        err = wait_for(hs, "error ", 2)
        report("8 errors reach the game", exited and bool(err) and bool(wait_for(hs, "stopped", 2)), "(%s)" % err)
    finally:
        for p in (host, join):
            if p and p.poll() is None:
                p.kill()
        srv.close()

    print("\n%d/%d checks passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
