import argparse
import json
import re
import time
from pathlib import Path


PIPE_PATH = r"\\.\pipe\wwmi-draw-debug"


def request(command: str) -> str:
    deadline = time.monotonic() + 3.0
    while True:
        try:
            with open(PIPE_PATH, "r+b", buffering=0) as pipe:
                pipe.write((command.rstrip() + "\n").encode("utf-8"))
                return pipe.read(4096).decode("utf-8", errors="replace").strip()
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)


def component_draws(ini_path: Path) -> list[tuple[int, int]]:
    text = ini_path.read_text(encoding="utf-8-sig", errors="replace")
    current = ""
    values: dict[str, str] = {}
    draws: set[tuple[int, int]] = set()

    def finish() -> None:
        if not current.lower().startswith("textureoverridecomponent"):
            return
        if "match_index_count" not in values:
            return
        count = int(values["match_index_count"], 0)
        first = int(values.get("match_first_index", "0"), 0)
        draws.add((count, first))

    for raw in text.splitlines():
        line = raw.split(";", 1)[0].strip()
        section = re.fullmatch(r"\[([^]]+)\]", line)
        if section:
            finish()
            current = section.group(1)
            values = {}
            continue
        if "=" in line:
            key, value = line.split("=", 1)
            values[key.strip().lower()] = value.strip()
    finish()
    return sorted(draws)


def arm_target(ini_path: Path) -> None:
    draws = component_draws(ini_path)
    if not draws:
        raise SystemExit(f"No component draw signatures found in {ini_path}")
    print(request("FILTER CLEAR"))
    for count, first in draws:
        response = request(f"FILTER DRAW {count} {first}")
        if not response.startswith("OK"):
            raise SystemExit(response)
    print(f"Loaded {len(draws)} targeted draw signatures from {ini_path}")
    print(request("ARM"))


def tail(follow: bool) -> None:
    status = json.loads(request("STATUS"))
    path = Path(status["path"])
    if not path:
        raise SystemExit("No active or previous stream path")
    position = 0
    while True:
        if path.exists():
            with path.open("r", encoding="utf-8", errors="replace") as stream:
                stream.seek(position)
                for line in stream:
                    print(line, end="")
                position = stream.tell()
        if not follow:
            return
        time.sleep(0.25)


def main() -> None:
    parser = argparse.ArgumentParser(description="WWMI Draw Debug control client")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("ping", "status", "start", "stop", "snapshot"):
        sub.add_parser(name)
    mark = sub.add_parser("mark")
    mark.add_argument("label")
    arm = sub.add_parser("arm")
    arm.add_argument("ini", type=Path)
    tail_parser = sub.add_parser("tail")
    tail_parser.add_argument("--follow", action="store_true")
    args = parser.parse_args()

    if args.command == "arm":
        arm_target(args.ini)
    elif args.command == "mark":
        print(request(f"MARK {args.label}"))
    elif args.command == "tail":
        tail(args.follow)
    else:
        print(request(args.command.upper()))


if __name__ == "__main__":
    main()
