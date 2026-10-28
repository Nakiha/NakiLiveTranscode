#!/usr/bin/env python3
"""Enforce application dependency and single-owner I/O boundaries without an SDK.

This guards accidental architectural drift, not hostile changes to this policy.
Private header consumers are explicit: a new consumer requires a policy review.
"""
import argparse
from pathlib import Path
import re

LAYERS = {
    'media_hooks.h': {'media'},  # Existing private hook factory compatibility shim.
    'control': {'control'},
    'platform': {'platform', 'control'},
    'observability': {'observability', 'crash'},
    'crash': {'crash'},
    'cli': set(),
    'controller': {'controller', 'control', 'platform', 'observability', 'crash', 'worker', 'media'},
    'worker': {'worker', 'control', 'platform', 'observability', 'crash', 'media'},
    'media': {'media', 'control', 'platform', 'observability', 'crash', 'worker'},
}
PRIVATE = {
    'media/mux_internal.h': {'media/pipeline_output.c', 'media/pipeline_audio.c', 'media/pipeline_mux.c'},
    'media/pipeline_internal.h': {
        'media/pipeline.c', 'media/pipeline_reader.c', 'media/pipeline_encode.c',
        'media/pipeline_decode.c', 'media/pipeline_decode_queue.c', 'media/mux_internal.h',
        'media/rendition_runtime.c', 'media/rendition_runtime_internal.h',
        'media/rendition_child.c', 'media/rendition_media.c', 'media/destination_runtime.c',
    },
    'media/rendition_runtime_internal.h': {'media/rendition_runtime.c', 'media/rendition_control.c'},
    'worker/job.h': {'worker/worker.c', 'worker/job.c', 'worker/job_report.c',
                     'worker/frame_policy_runtime.c', 'media/pipeline_internal.h'},
    'controller/service_internal.h': {'controller/service.c', 'controller/service_http.c',
        'controller/service_supervisor.c', 'controller/service_projection.c'},
}
OWNERS = {
    'av_write_frame': {'media/pipeline_mux.c'},
    'av_interleaved_write_frame': set(),
    'avformat_live_add_audio': {'media/pipeline_audio.c'},
    'avformat_live_mux_init': {'media/pipeline_output.c'},
    'avformat_live_mux_send': {'media/pipeline_mux.c'},
    'avformat_live_mux_receive': {'media/pipeline_mux.c'},
    'avformat_alloc_output_context2': {'media/pipeline_output.c'},
    'avformat_write_header': {'media/pipeline_output.c'},
    'av_write_trailer': {'media/pipeline_output.c'},
    'avio_open2': {'media/pipeline_output.c'},
    'avio_closep': {'media/pipeline_output.c'},
}
TOKEN = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
INCLUDE = re.compile(r'^\s*#\s*include\s+([<"])([^>"\n]+)[>"]', re.M)

def comments_removed(text):
    # Preserve newlines for diagnostics and preserve quoted include operands.
    return TOKEN.sub(lambda m: re.sub(r'[^\n]', ' ', m[0]) if m[0].startswith(('/',)) else m[0], text)

def code_only(text):
    return TOKEN.sub(lambda m: re.sub(r'[^\n]', ' ', m[0]), text)

def check(root):
    root = Path(root).resolve()
    app = root / 'app'
    files = {p.relative_to(app).as_posix(): p for p in app.rglob('*') if p.suffix in ('.c', '.h')}
    errors, edges = [], {}
    def fail(name, why): errors.append(f'app/{name}: {why}')
    for name, path in sorted(files.items()):
        text = path.read_text(encoding='utf-8')
        clean = comments_removed(text.replace('\\\n', ''))
        layer = name.split('/')[0]
        if layer not in LAYERS:
            fail(name, 'unregistered component'); continue
        edges[name] = []
        for directive in re.findall(r'^\s*#\s*include\s+([^\n]+)', clean, re.M):
            if not re.match(r'[<"][^>"\n]+[>"]\s*$', directive):
                fail(name, 'computed include bypasses dependency policy')
        for match in INCLUDE.finditer(clean):
            quote, target = match.groups()
            local = next((p.resolve() for p in (path.parent / target, app / target) if p.is_file()), None)
            if local and local.is_relative_to(app):
                child = local.relative_to(app).as_posix()
                edges[name].append(child)
                if child.endswith('.c'): fail(name, 'implementation include forbidden: ' + child)
                if child.split('/')[0] not in LAYERS[layer]: fail(name, 'forbidden dependency: ' + child)
                if child in PRIVATE and name not in PRIVATE[child]: fail(name, 'private header: ' + child)
                if layer == 'controller' and child.startswith('media/') and child != 'media/media_hooks.h':
                    fail(name, 'controller may only consume the media hook capability contract')
            elif target.startswith('libav') and layer == 'control':
                fail(name, 'control must not depend on the native media SDK')
            elif quote == '"' and not target.startswith('libav'):
                fail(name, 'unresolved or out-of-app quoted include: ' + target)
        # Function-like macros and taking function addresses also count as uses.
        code = code_only(text)
        if name != 'media/mux_internal.h' and re.search(r'\bstruct\s+NakiMux\s*\{', code):
            fail(name, 'mux representation is private')
        for symbol, owners in OWNERS.items():
            if re.search(r'\b' + symbol + r'\b', code) and name not in owners:
                fail(name, 'single-owner API ' + symbol)
    # Follow local includes: a harmless-looking wrapper cannot leak mutable state.
    for name in edges:
        if name.startswith('controller/'):
            forbidden = {'worker/job.h', 'media/pipeline_internal.h', 'media/mux_internal.h'}
        elif name.startswith('control/') or name in ('media/rendition_runtime.h', 'media/rendition_media.h',
                'media/frame_wire.c', 'media/frame_trace.h', 'media/audio_wire.c',
                'media/shared_frame_pool.c', 'media/rendition_message.c', 'media/session_idle.c'):
            forbidden = {'worker/job.h', 'media/pipeline_internal.h', 'media/mux_internal.h'}
        else:
            continue
        pending, seen = list(edges[name]), set()
        while pending:
            child = pending.pop()
            if child in seen: continue
            seen.add(child)
            if child in forbidden: fail(name, 'transitive private state: ' + child)
            pending.extend(edges.get(child, ()))
    cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
    for args in re.findall(r'target_link_libraries\s*\(\s*naki-control\s+([^)]*)\)', cmake):
        if re.search(r'FFMPEG|libav', args): errors.append('CMakeLists.txt: naki-control links media SDK')
    return errors

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = check(args.root)
    if errors:
        print('\n'.join(errors))
        return 1
    print('PASS architecture: dependencies, private state and mux I/O owners')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
