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
    'media/input_internal.h': {'media/pipeline_input.c','media/pipeline_reader.c','media/pipeline_decode.c','media/pipeline_decode_queue.c'},
    'media/video_internal.h': {'media/pipeline_video.c','media/pipeline_encode.c'},
    'media/mux_internal.h': {'media/pipeline_output.c', 'media/pipeline_audio.c', 'media/pipeline_mux.c'},
    'media/pipeline_internal.h': {
        'media/pipeline.c', 'media/pipeline_reader.c', 'media/pipeline_encode.c',
        'media/pipeline_input.c','media/pipeline_video.c','media/pipeline_branches.c',
        'media/input_internal.h','media/video_internal.h',
        'media/pipeline_decode.c', 'media/pipeline_decode_queue.c', 'media/mux_internal.h',
        'media/rendition_runtime.c', 'media/rendition_runtime_internal.h',
        'media/rendition_media.c', 'media/destination_runtime.c',
    },
    'media/rendition_runtime_internal.h': {'media/rendition_runtime.c', 'media/rendition_control.c'},
    'worker/job.h': {'worker/worker.c', 'worker/job.c', 'worker/job_report.c',
                     'worker/job_media.c', 'worker/job_remote.c', 'worker/frame_policy_runtime.c'},
    'controller/service_internal.h': {'controller/service.c', 'controller/service_http.c',
        'controller/service_supervisor.c', 'controller/service_projection.c'},
}
OWNERS = {
    'avformat_open_input': {'media/pipeline_input.c'},
    'avformat_close_input': {'media/pipeline_input.c'},
    'av_read_frame': {'media/pipeline_reader.c'},
    'avcodec_send_frame': {'media/pipeline_encode.c'},
    'avcodec_receive_packet': {'media/pipeline_encode.c'},
    'avcodec_async_free': {'media/pipeline_input.c'},
    'avcodec_async_receive_frame': {'media/pipeline_decode.c','media/decode_backend.h'},
    'avcodec_async_send_packet': {'media/pipeline_decode.c','media/decode_backend.h'},
    'avcodec_parameters_from_context': {'media/encoded_description.c'},
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
# Event authorities: declarations are allowed in the contract; state changes
# stay on the actual resource/topology owner. Pure views remain freely usable.
EVENT_OWNERS = {
    'naki_encoded_seal': {'media/pipeline_video.c','media/encoded_description.c','media/encoded_description.h'},
    'naki_topology_advance': {'media/rendition_control.c','media/rendition_lifecycle.c','media/rendition_lifecycle.h'},
}
for event in ('begin','ready','finished_event','fail','reaped'):
    EVENT_OWNERS['naki_rl_'+event]={'media/rendition_runtime.c','media/rendition_lifecycle.c','media/rendition_lifecycle.h'}
for event in ('restart','remove','attach','clear'):
    EVENT_OWNERS['naki_rl_'+event]={'media/rendition_control.c','media/rendition_lifecycle.c','media/rendition_lifecycle.h'}
for event in ('reserve','stop'):
    EVENT_OWNERS['naki_rl_'+event]={'media/rendition_runtime.c','media/rendition_control.c','media/rendition_lifecycle.c','media/rendition_lifecycle.h'}
for event in ('reserve','started','ready','attach','remove','joined','clear'):
    EVENT_OWNERS['naki_dl_'+event]={'media/destination_runtime.c','media/destination_lifecycle.c','media/destination_lifecycle.h'}
for event in ('init','ready','stop','fail','finish'):
    EVENT_OWNERS['naki_writer_'+event]={'media/destination_runtime.c','media/destination_lifecycle.c','media/destination_lifecycle.h'}
for event in ('init','open','quiesce','clear','release_decoder','close_demux','destroy'):
    EVENT_OWNERS['pipeline_input_'+event]={'media/pipeline.c','media/pipeline_input.c','media/pipeline_internal.h'}
for event in ('close','release'):
    EVENT_OWNERS['pipeline_video_'+event]={'media/pipeline.c','media/pipeline_video.c','media/pipeline_internal.h'}
for event in ('init','join','clear','destroy'):
    EVENT_OWNERS['pipeline_reader_'+event]={'media/pipeline_input.c','media/pipeline_reader.c','media/input_internal.h'}
EVENT_OWNERS['pipeline_decode_start']={'media/pipeline.c','media/pipeline_decode.c','media/pipeline_internal.h'}
EVENT_OWNERS['pipeline_reader_start']={'media/pipeline.c','media/pipeline_reader.c','media/pipeline_reader.h'}

JOB_PUBLISHERS = {
    'naki_job_frame_written': {'media/pipeline_mux.c'},
    'naki_job_complete': {'media/pipeline_output.c'},
    'naki_job_finalize_begin': {'media/pipeline_output.c'},
    'naki_job_finalize_result': {'media/pipeline_output.c'},
    'naki_job_remote_slots': {'media/rendition_runtime.c'},
    'naki_job_bind_renditions': {'media/rendition_runtime.c'},
    'naki_job_remote_configure': {'media/rendition_control.c'},
    'naki_job_remote_forget': {'media/rendition_control.c'},
    'naki_job_remote_spawned': {'media/rendition_runtime.c'},
    'naki_job_remote_exit_observed': {'media/rendition_runtime.c'},
    'naki_job_remote_retired': {'media/rendition_runtime.c'},
    'naki_job_remote_project': {'media/rendition_runtime.c'},
    'naki_job_destination_new': {'media/destination_runtime.c'},
    'naki_job_destination_free': {'media/destination_runtime.c'},
    'naki_job_destinations_publish': {'media/destination_runtime.c'},
    'naki_job_destination_written': {'media/destination_runtime.c'},
    'naki_job_child_new': {'media/rendition_child.c'},
    'naki_job_child_logger': {'media/rendition_child.c'},
    'naki_job_child_identity': {'media/rendition_child.c'},
    'naki_job_child_resources': {'media/rendition_child.c'},
    'naki_job_child_trace': {'media/rendition_child.c'},
    'naki_job_child_stopped': {'media/rendition_child.c'},
    'naki_job_child_free': {'media/rendition_child.c'},
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
                if layer == 'controller' and child.startswith('worker/') and child not in {'worker/job_api.h', 'worker/job_state.h'}:
                    fail(name, 'controller may not invoke worker media publication/factories')
            elif target.startswith('libav') and layer == 'control':
                fail(name, 'control must not depend on the native media SDK')
            elif quote == '"' and not target.startswith('libav'):
                fail(name, 'unresolved or out-of-app quoted include: ' + target)
        # Function-like macros and taking function addresses also count as uses.
        code = code_only(text)
        if name != 'media/mux_internal.h' and re.search(r'\bstruct\s+NakiMux\s*\{', code):
            fail(name, 'mux representation is private')
        for typename, owner in [('NakiInput','media/input_internal.h'),('NakiVideo','media/video_internal.h'),
                                ('NakiEncodedDescription','media/encoded_description.c'),('NakiBranches','media/pipeline_branches.c'),
                                ('Pipeline','media/pipeline_internal.h'),('PipelineReader','media/input_internal.h'),
                                ('DecodePump','media/input_internal.h'),('Ledger','media/video_internal.h'),('Branch','media/pipeline_branches.c')]:
            if name != owner and re.search(r'\bstruct\s+'+typename+r'\s*\{', code):
                fail(name, typename+' representation is private')
        if name == 'media/pipeline_internal.h':
            state = re.search(r'\bstruct\s+Pipeline\s*\{(.*?)\}\s*Pipeline\b',code,re.S)
            if state and re.search(r'\b(?:AVCodecContext|AVCodecAsyncContext|AVFormatContext|AVStream|AVFilterGraph|AVFilterContext|AVIOContext|AVBSFContext|AVFrame|AVPacket|PipelineReader|DecodePump|Ledger|NakiBranchQueue|NakiLiveRate)\b',state[1]):
                fail(name,'orchestration may only hold opaque component handles and timing descriptions')
        if not name.startswith('worker/') and re.search(r'\bstruct\s+Job\s*\{', code):
            fail(name, 'worker Job representation is private')
        for token, allowed in {
            '_private_life': {'media/rendition_lifecycle.c','media/rendition_lifecycle.h'},
            '_private_writer': {'media/destination_lifecycle.c','media/destination_lifecycle.h'},
            '_private_destination': {'media/destination_lifecycle.c','media/destination_lifecycle.h'},
        }.items():
            if name not in allowed and re.search(r'\b'+token+r'\b',code):
                fail(name,'private lifecycle storage')
        for symbol, owners in EVENT_OWNERS.items():
            if re.search(r'\b'+symbol+r'\b',code) and name not in owners:
                fail(name,'unauthorized owner event '+symbol)
        for symbol, owners in OWNERS.items():
            if re.search(r'\b' + symbol + r'\b', code) and name not in owners:
                fail(name, 'single-owner API ' + symbol)
        if not name.startswith('worker/'):
            for symbol, owners in JOB_PUBLISHERS.items():
                if re.search(r'\b' + symbol + r'\b', code) and name not in owners:
                    fail(name, 'unauthorized Job publisher/factory ' + symbol)
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
    if 'naki-live-worker' in cmake:
        for component, expected in {
            'input': {'pipeline_input.c','pipeline_reader.c','pipeline_decode.c','pipeline_decode_queue.c'},
            'video': {'pipeline_video.c','pipeline_encode.c'},
            'output': {'pipeline_output.c','pipeline_mux.c','pipeline_audio.c'},
        }.items():
            match = re.search(r'add_library\s*\(\s*naki-media-'+component+r'\s+STATIC\s+([^)]*)\)',cmake)
            actual = set(match[1].split()) if match else set()
            if actual != {'app/media/'+p for p in expected}:
                errors.append('CMakeLists.txt: component source ownership naki-media-'+component)
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
