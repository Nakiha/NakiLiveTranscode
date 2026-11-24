"""Reconcile actual VTM choices and FFmpeg SAO headers with the frozen Tencent traces."""
from collections import Counter, defaultdict
import gzip
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
CATEGORIES = ("tu_geometry", "parameter_by_shape", "block_tool", "selected_reference", "sao_enabled")
FIELDS = re.compile(rb"\[trace_headers @ [^\]]+\]\s+\d+\s+(\w+)\s+[01]+ = (-?\d+)\s*$")
WANTED = {"ph_pic_order_cnt_lsb", "sps_log2_max_pic_order_cnt_lsb_minus4", "sps_bitdepth_minus8",
          "sps_sao_enabled_flag", "sh_sao_luma_used_flag", "sh_sao_chroma_used_flag"}


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def reduce_trace(source, stage):
    cmd = ["awk", "-v", "stage=" + stage, "-f", HERE / "r28-tools.awk"]
    if stage != "use":
        cmd += ["-f", HERE / "r28-geometry.awk"]
    result = subprocess.run(cmd, stdin=source, capture_output=True, check=False)
    if result.returncode:
        raise ValueError(result.stderr.decode(errors="replace").strip() or "invalid VTM trace")
    counts = defaultdict(Counter)
    for line in result.stdout.decode().splitlines():
        group, key, n = line.split("\t")
        counts[group][key] = int(n)
    return counts


def read_fields(lines):
    fields = defaultdict(list)
    for line in lines:
        m = FIELDS.search(line)
        if m:
            key = m[1].decode()
            if key in WANTED:
                fields[key].append(int(m[2]))
    return fields


def unique(fields, key):
    values = set(fields.get(key, []))
    if len(values) != 1:
        raise ValueError(f"missing or changing {key}")
    return values.pop()


def sao_choices(fields, ordered):
    order = sorted((int(key.split("/")[0]), key.split("/")[1], poc) for key, poc in ordered.items())
    n = len(order)
    modulus = 1 << (4 + unique(fields, "sps_log2_max_pic_order_cnt_lsb_minus4"))
    if [i for i, _, _ in order] != list(range(n)) or fields["ph_pic_order_cnt_lsb"] != [p % modulus for _, _, p in order]:
        raise ValueError("FFmpeg picture order does not match VTM POCs")
    enabled = unique(fields, "sps_sao_enabled_flag")
    result = Counter()
    for field, plane in (("sh_sao_luma_used_flag", "Y"), ("sh_sao_chroma_used_flag", "C")):
        values = fields[field]
        if enabled == 0 and not values:
            values = [0] * n
        if len(values) != n or any(v not in (0, 1) for v in values):
            raise ValueError(f"incomplete {field}")
        for (_, role, _), v in zip(order, values):
            if v:
                result[role + "/" + plane] += 1
    return result


def original_counts(directory, proof, geometry):
    counts = defaultdict(Counter)
    headers = {}
    for sample, information in proof["headers"].items():
        file = directory / information["file"]
        if digest(file) != information["sha256"]:
            raise ValueError(f"unexpected original SAO trace {file}")
        with gzip.open(file, "rb") as src:
            headers[sample] = read_fields(src)

    orders = defaultdict(dict)
    for original, usage in zip(geometry["sources"], proof["usage"], strict=True):
        f = directory / "structure" / original["trace"]
        u = directory / usage["file"]
        if digest(f) != original["trace_sha256"] or digest(u) != usage["sha256"]:
            raise ValueError(f"unexpected original VTM trace {f} or {u}")
        # A subprocess needs a decompressed pipe, not the descriptor of a gzip.GzipFile.
        stages = []
        for file, stage in ((f, "structure"), (u, "use")):
            with subprocess.Popen(["gzip", "-dc", "--", file], stdout=subprocess.PIPE) as unzip:
                try:
                    stages.append(reduce_trace(unzip.stdout, stage))
                finally:
                    unzip.stdout.close()
                if unzip.wait():
                    raise ValueError(f"invalid gzip trace {file}")
        struct, actual = stages
        if (struct["coverage"]["pictures"] != original["frames"] or struct["coverage"]["periods"] != 1 or
                struct["decode_order"] != actual["decode_order"]):
            raise ValueError("original picture identities differ between the two probes")
        sample = usage["sample"]
        offset = len(orders[sample])
        for key, poc in struct["decode_order"].items():
            i, role = key.split("/")
            orders[sample][f"{offset + int(i)}/{role}"] = poc
        for group in CATEGORIES:
            counts[group].update(struct[group])
            counts[group].update(actual[group])
    for sample, order in orders.items():
        counts["sao_enabled"].update(sao_choices(headers[sample], order))
    return dict(counts)


def executable(path):
    if Path(path).is_file():
        return Path(path).resolve()
    resolved = shutil.which(str(path))
    if resolved is None:
        raise ValueError(f"missing executable {path}")
    return Path(resolved).resolve()


def decode(stream, decoder, ffmpeg, trace):
    with subprocess.Popen([trace, "-nostdin", "-hide_banner", "-loglevel", "info", "-xerror", "-f", "vvc",
                           "-i", stream, "-map", "0:v", "-c:v", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as proc:
        fields = read_fields(proc.stderr)
        if proc.wait():
            raise ValueError("FFmpeg header trace failed")
    depth = 8 + unique(fields, "sps_bitdepth_minus8")
    if depth not in (8, 10):
        raise ValueError(f"unsupported R28 bit depth {depth}")
    with tempfile.TemporaryDirectory() as tmp:
        reconstruction = Path(tmp) / "vtm.yuv"
        with subprocess.Popen([decoder, "-b", stream, "-o", reconstruction, f"--OutputBitDepth={depth}"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as proc:
            try:
                counts = reduce_trace(proc.stderr, "combined")
            finally:
                proc.stderr.close()
            if proc.wait() or reconstruction.stat().st_size == 0:
                raise ValueError("VTM reconstruction failed")
        vtm_hash = digest(reconstruction)
        pixel_format = "yuv420p" if depth == 8 else "yuv420p10le"
        independent = Path(tmp) / "ffmpeg.yuv"
        subprocess.run([ffmpeg, "-nostdin", "-v", "error", "-xerror", "-threads", "2", "-f", "vvc",
                        "-i", stream, "-map", "0:v", "-fps_mode", "passthrough", "-pix_fmt", pixel_format,
                        "-threads", "1", "-f", "rawvideo", independent], check=True)
        ffmpeg_hash = digest(independent)
    counts["sao_enabled"] = sao_choices(fields, counts.pop("decode_order"))
    return counts, dict(bit_depth=depth, vtm_sha256=vtm_hash, ffmpeg_sha256=ffmpeg_hash)
