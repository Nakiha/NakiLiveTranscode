"""Read only process-event active/sealed paths; never traces or damaged recovery files."""
import json
import pathlib


def read_events(directory):
    result=[]
    for path in sorted(pathlib.Path(directory).glob("*.events.*.jsonl*")):
        if not (path.name.endswith(".jsonl") or path.name.endswith(".jsonl.active")):continue
        try:data=path.read_bytes()
        except FileNotFoundError:continue  # concurrent active->sealed rename; the next snapshot observes it
        for line in data.splitlines(keepends=True):
            if not line.endswith(b"\n"):continue
            result.append(json.loads(line))
    return result
