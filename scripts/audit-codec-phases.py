#!/usr/bin/env python3
"""Validate picture correlation and separate codec residence from thread CPU.

Consumes sealed application traces only. No codec-thread logging is required.
CPU sums from parallel CTU tasks are never subtracted from elapsed wall time.
"""
import argparse
import json
import math
import pathlib
import statistics

GOP_CHECKS = ['gop_order_blocks','gop_analysis_blocks','gop_ratecap_blocks',
              'gop_reference_blocks','gop_aps_blocks','gop_encoder_blocks','gop_rc_blocks','gop_visits']
# Independent monotonic and per-thread CPU clocks are not an exact subtraction
# pair. v2 decoder audits use the unslewed RAW enclosure, keeping this original
# quantization/readout allowance. Legacy decoder/encoder traces retain their
# original strict MONOTONIC test; historical failures must not be regraded.
CPU_CLOCK_ALLOWANCE_US = 5


def summary(values):
    values = sorted(values)
    if not values:
        return dict(count=0)
    return dict(count=len(values), mean=statistics.mean(values), max=values[-1],
                p50=values[math.ceil(len(values)*.5)-1],
                p95=values[math.ceil(len(values)*.95)-1],
                p99=values[math.ceil(len(values)*.99)-1])


def ordered(*times):
    if any(not isinstance(t, int) or t <= 0 for t in times) or list(times) != sorted(times):
        raise ValueError(f'Invalid codec event order: {times}')


def bounded_cpu_us(cpu, begin, end):
    assert isinstance(cpu,int) and cpu>=0 and end>=begin
    assert cpu<=end-begin+CPU_CLOCK_ALLOWANCE_US, 'Single-thread CPU interval exceeds declared cross-clock allowance'
    return max(0, cpu-(end-begin))


def decode_parts(event):
    d = event['decoder_internal']
    assert d['version'] in (1, 2)
    ordered(event['decoder_submit_monotonic_us'], d['dispatch_us'], d['worker_start_us'],
            d['decode_begin_us'], d['picture_start_us'], d['reconstructed_us'])
    # HEVC can nominate a DPB output before reconstruction completes. It is
    # output eligibility, not permission to read unfinished reconstructed pixels.
    ready = max(d['reconstructed_us'], d['output_selected_us'])
    ordered(d['picture_start_us'], d['output_selected_us'])
    # worker_output is a private worker queue publication. A different worker
    # can nominate/output this picture while its original worker is finishing.
    # The coordinator collects completed workers in submission order; the
    # async output queue is the externally visible join of these branches.
    ordered(d['output_selected_us'], d['worker_output_us'])
    delivered = max(ready, d['worker_output_us'])
    ordered(delivered, d['async_queued_us'], d['frontend_us'], event['decoded_monotonic_us'])
    assert d['decode_wall_us'] == d['reconstructed_us']-d['decode_begin_us']
    # MONOTONIC is slewed by timekeeping discipline; Linux thread CPU accounting
    # need not share that rate. v2 measures an unslewed, enclosing RAW interval.
    # Keep v1's original strict behavior for historical evidence, never regrade it.
    cpu_wall = d['decode_cpu_wall_us'] if d['version'] >= 2 else d['decode_wall_us']
    assert isinstance(cpu_wall, int) and cpu_wall > 0
    excess = bounded_cpu_us(d['decode_cpu_us'], 0, cpu_wall)
    assert 0 <= d['reference_wait_us'] <= d['decode_wall_us']
    timeline = [event['decoder_submit_monotonic_us'], d['dispatch_us'], d['worker_start_us'],
                d['decode_begin_us'], d['reconstructed_us'], ready, delivered,
                d['async_queued_us'], d['frontend_us'], event['decoded_monotonic_us']]
    names = ['async_admission', 'worker_launch', 'before_decode', 'decode_window',
             'display_reorder', 'worker_delivery', 'async_transfer', 'frontend_queue', 'app_handoff']
    result = {n: (b-a)/1000 for n, a, b in zip(names, timeline, timeline[1:])}
    result.update(worker_output_lead=max(0, ready-d['worker_output_us'])/1000,
                  decode_cpu=d['decode_cpu_us']/1000, single_thread_cpu_clock_excess=excess/1000,
                  reference_wait=d['reference_wait_us']/1000,
                  cpu_audit_wall=cpu_wall/1000,
                  cpu_monotonic_excess=max(0, d['decode_cpu_us']-d['decode_wall_us'])/1000,
                  decode_off_cpu=max(0, cpu_wall-d['decode_cpu_us'])/1000)
    return result


def encode_parts(event):
    e = event['encoder_internal']
    assert e['version'] in (2,3) and e['task_calls'] > 0
    ordered(event['encoder_submit_monotonic_us'], e['input_us'], e['group_last_input_us'],
            e['group_formed_us'], e['preprocess_begin_us'], e['group_ready_us'],
            e['mctf_begin_us'], e['mctf_end_us'], e['dispatch_us'],
            e['ctu_done_us'], e['finalize_begin_us'], e['finalize_end_us'], e['reconstructed_us'], e['au_us'],
            event['encoded_monotonic_us'])
    ordered(e['dispatch_us'],e['task_first_us'],e['ctu_done_us'],e['finalize_begin_us'])
    ordered(e['dispatch_us'],e['compress_return_us'],e['finalize_begin_us'])
    times = [event['encoder_submit_monotonic_us'], e['input_us'], e['group_last_input_us'],
             e['group_formed_us'], e['preprocess_begin_us'], e['group_ready_us'],
             e['mctf_begin_us'], e['mctf_end_us'], e['dispatch_us'], e['task_first_us'], e['ctu_done_us'],
             e['finalize_begin_us'], e['finalize_end_us'], e['reconstructed_us'], e['au_us'], event['encoded_monotonic_us']]
    names = ['wrapper_input', 'group_future_input', 'group_formation', 'preprocess_queue',
             'preprocess', 'mctf_queue', 'mctf', 'gop_dispatch_wait', 'ctu_dispatch_wait', 'ctu_work_span',
             'finalize_dispatch_wait', 'finalize', 'reconstruction_publish', 'au_wait_write', 'wrapper_output']
    result = {n: (b-a)/1000 for n, a, b in zip(names, times, times[1:])}
    ordered(e['mctf_end_us'],e['gop_enter_us'],e['gop_init_begin_us'],e['gop_init_end_us'],e['dispatch_us'])
    assert 0<=e['gop_previous_done_us']<=e['dispatch_us'] and e['gop_visits']>0
    start,begin,end,dispatch,previous=(e[k] for k in ['gop_enter_us','gop_init_begin_us','gop_init_end_us','dispatch_us','gop_previous_done_us'])
    pre=max(0,min(begin,previous)-start)
    post=max(0,min(dispatch,previous)-end)
    detail=dict(gop_handoff=start-e['mctf_end_us'],gop_preinit_predecessor=pre,
                gop_preinit_available=begin-start-pre,gop_init=end-begin,
                gop_postinit_predecessor=post,gop_postinit_available=dispatch-end-post)
    assert sum(detail.values())==e['dispatch_us']-e['mctf_end_us']
    result.update({k:v/1000 for k,v in detail.items()})
    for key in GOP_CHECKS:
        assert e[key]>=0
    excesses = []
    for cpu,begin,end in [('gop_init_cpu_us','gop_init_begin_us','gop_init_end_us'),
                          ('preprocess_cpu_us','preprocess_begin_us','group_ready_us'),
                          ('dispatch_cpu_us','dispatch_us','compress_return_us'),
                          ('finalize_cpu_us','finalize_begin_us','finalize_end_us')]:
        excesses.append(bounded_cpu_us(e[cpu],e[begin],e[end]))
    result['single_thread_cpu_clock_excess'] = max(excesses)/1000
    if e['version']>=3:
        assert 0<=e['byte_reference_pack_us']<=e['finalize_end_us']-e['finalize_begin_us']+2
    result['gop_init_cpu']=e['gop_init_cpu_us']/1000
    for name in ['preprocess_cpu_us', 'dispatch_cpu_us', 'finalize_cpu_us', 'ctu_cpu_us',
                 'deblock_cpu_us', 'sao_cpu_us', 'alf_cpu_us', 'finish_cpu_us', 'task_wall_us']:
        assert e[name] >= 0
        result[name.removesuffix('_us')] = e[name]/1000
    return result


def audit(directory):
    state = json.loads((directory/'final-state.json').read_text())
    assert state['codec_trace']
    prefix = pathlib.Path(state['log_path'])
    assert not list(prefix.parent.glob(prefix.name+'.*.active')), 'Unsealed trace'
    files = sorted(prefix.parent.glob(prefix.name+'.*.jsonl'))
    assert files
    decoded, frames = {}, []
    for path in files:
        assert path.stat().st_size <= 128*1024*1024
        for line in path.open():
            e = json.loads(line)
            if e.get('event') == 'decoder_frame':
                key = e['frame_id']
                assert key not in decoded, 'Duplicate decoder picture'
                decoded[key] = e
                decode_parts(e)
            elif e.get('event') == 'frame':
                frames.append(e)
    assert len(frames) == state['packets_out'] and frames
    assert len(decoded) >= state['frames_decoded']
    assert len(decoded) <= state['frames_decoded']+1, 'Unaccounted decoded pictures'
    for k, v in state['logging'].items():
        if ('dropped' in k or 'error' in k) and isinstance(v, (int, float)):
            assert v == 0, (k, v)
    groups = {}
    previous=0
    for f in sorted(frames,key=lambda f:f['encoder_internal']['dispatch_us']):
        assert f['encoder_internal']['gop_previous_done_us']==previous, 'Broken serial predecessor identity'
        previous=f['encoder_internal']['reconstructed_us']
    for kind in ['all', 'I', 'P', 'B']:
        selected = [f for f in frames if kind == 'all' or f['output_picture_type'] == kind]
        dec, enc = [], []
        for f in selected:
            d = decoded[f['frame_id']]
            assert f['decoder_internal'] == d['decoder_internal'] and f['pts_us'] == d['pts_us']
            dec.append(decode_parts(d));enc.append(encode_parts(f))
        groups[kind] = dict(count=len(selected),
                            decoder_ms={k: summary([r[k] for r in dec]) for k in dec[0]} if dec else {},
                            encoder_ms={k: summary([r[k] for r in enc]) for k in enc[0]} if enc else {},
                            gop_checks={k:dict(total=sum(f['encoder_internal'][k] for f in selected),
                                               **summary([f['encoder_internal'][k] for f in selected])) for k in GOP_CHECKS})
    return dict(status='passed', decoded_trace_frames=len(decoded), written_frames=len(frames), groups=groups,
                single_thread_cpu_clock_allowance_us=CPU_CLOCK_ALLOWANCE_US,
                decoder_cpu_audit_clock='v2: CLOCK_MONOTONIC_RAW enclosure; v1: original CLOCK_MONOTONIC rule',
                cpu_scope='Decoder calling thread; VVenC successful CTU task thread CPU summed across workers. MCTF worker CPU is not included.',
                wall_scope='Residence includes dependency waits and scheduling; CPU sums overlap elapsed intervals. Private worker output can precede picture reconstruction; async queue joins the branches. worker_output_lead is overlap, not an additive stage.',
                cancellation_tail='A prepared unconsumed dispatch slot may add one decoder event after API stop.')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=pathlib.Path)
    p.add_argument('--output', type=pathlib.Path, required=True)
    args = p.parse_args()
    result = audit(args.directory)
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
    print('PASS codec phases:', args.output)
