#!/usr/bin/env python3
"""Talk to the DayZ VR debug plugin while DayZ is running.

The plugin (dayz_openxr_debug.dll, enabled by [debug] in dayz_openxr.ini) listens on
127.0.0.1:<port> inside the Proton/Wine process, which is the same loopback as the
host, so this script needs no Wine. Examples:

    dayz-vr-ctl.py get                      # full state as JSON
    dayz-vr-ctl.py get hmd_yaw host_fps     # just some fields, one per line
    dayz-vr-ctl.py watch --interval 0.5     # repeating one-line summary
    dayz-vr-ctl.py tunables                 # current tunable values
    dayz-vr-ctl.py set stereo.hmd_mouse_yaw_scale -300
    dayz-vr-ctl.py recenter
"""

from __future__ import annotations

import argparse
import json
import math
import socket
import sys
import time
from typing import Any

DEFAULT_PORT = 48621


class DebugClient:
    def __init__(self, host: str, port: int, timeout: float) -> None:
        self._socket = socket.create_connection((host, port), timeout=timeout)
        self._reader = self._socket.makefile("r", encoding="utf-8", newline="\n")

    def close(self) -> None:
        self._reader.close()
        self._socket.close()

    def request(self, line: str) -> Any:
        self._socket.sendall((line + "\n").encode("utf-8"))
        reply = self._reader.readline()
        if not reply:
            raise ConnectionError("the plugin closed the connection")
        return json.loads(reply)


def degrees(radians: float) -> float:
    return math.degrees(radians)


def summary(state: dict[str, Any]) -> str:
    hand = state["right_hand"]
    return (
        f"fps={state['host_fps']:5.1f} state={state['session_state']} "
        f"focus={'y' if state['window_focused'] else 'n'} "
        f"gui={'y' if state['gui_cursor_mode'] else 'n'} eye={state['rendered_eye']} "
        f"yaw={degrees(state['hmd_yaw']):7.1f} pitch={degrees(state['hmd_pitch']):6.1f} "
        f"roll={degrees(state['hmd_roll']):6.1f} "
        f"pos=({state['hmd_position'][0]:.3f},{state['hmd_position'][1]:.3f},{state['hmd_position'][2]:.3f}) "
        f"mouse=({state['pending_mouse_x']:.1f},{state['pending_mouse_y']:.1f}) "
        f"aimR={'ok' if hand['aim_valid'] else '--'} presents={state['present_count']}"
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"[debug] port from the ini (default {DEFAULT_PORT})")
    parser.add_argument("--timeout", type=float, default=3.0)
    commands = parser.add_subparsers(dest="command", required=True)
    get = commands.add_parser("get", help="print the state, or selected fields")
    get.add_argument("fields", nargs="*")
    watch = commands.add_parser("watch", help="print a one-line summary repeatedly")
    watch.add_argument("--interval", type=float, default=1.0)
    watch.add_argument("--count", type=int, default=0, help="stop after this many samples (0 = until Ctrl-C)")
    commands.add_parser("tunables", help="list tunables and their current values")
    setter = commands.add_parser("set", help="change a tunable for the running game")
    setter.add_argument("name")
    setter.add_argument("value", type=float)
    commands.add_parser("recenter", help="recapture the HMD yaw and position centre")
    commands.add_parser("ping")
    args = parser.parse_args(argv)

    try:
        client = DebugClient(args.host, args.port, args.timeout)
    except OSError as error:
        print(f"cannot connect to {args.host}:{args.port}: {error}. Is DayZ running with [debug] enabled=true?", file=sys.stderr)
        return 2
    try:
        if args.command == "get":
            state = client.request("get")
            if args.fields:
                for field in args.fields:
                    print(f"{field}={json.dumps(state.get(field))}")
            else:
                print(json.dumps(state, indent=2))
        elif args.command == "watch":
            samples = 0
            while args.count == 0 or samples < args.count:
                print(summary(client.request("get")), flush=True)
                samples += 1
                if args.count == 0 or samples < args.count:
                    time.sleep(args.interval)
        elif args.command == "tunables":
            for name, value in client.request("tunables").items():
                print(f"{name}={value}")
        elif args.command == "set":
            reply = client.request(f"set {args.name} {args.value!r}")
            print(json.dumps(reply))
            return 0 if reply.get("ok") else 1
        elif args.command == "recenter":
            print(json.dumps(client.request("recenter")))
        elif args.command == "ping":
            print(json.dumps(client.request("ping")))
    except KeyboardInterrupt:
        pass
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
