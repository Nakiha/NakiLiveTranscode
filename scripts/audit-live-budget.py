#!/usr/bin/env python3
"""Audit complete native RTMP provenance traces; violations never mean success.

No shaping or rate-control is performed. Fixed cap is audited over half-open
rolling one-second output DTS and write-clock windows. Source following uses
two independent audit buckets, driven by completed RTMP messages, with explicit
finite burst and zero initial credit. Video config bytes are charged once.
"""
import argparse
import collections
import json
import math
import pathlib
import statistics


def summary(values):
    values = sorted(values)
    if not values:
        return dict(count=0, mean=None, max=None)
    return dict(count=len(values), mean=statistics.mean(values), max=values[-1],
                p50=values[math.ceil(len(values)*.5)-1], p95=values[math.ceil(len(values)*.95)-1], p99=values[math.ceil(len(values)*.99)-1])


def rolling(events, limit):
    queue, total, peak, exceeded = collections.deque(), 0, 0, 0
    samples = []
    for time, bits in sorted(events,key=lambda event:event[0]):
        while queue and queue[0][0] <= time-1000000:
            total -= queue.popleft()[1]
        queue.append((time,bits)); total += bits
        peak = max(peak,total); exceeded += total > limit
        samples.append(dict(time_us=time, bits_in_last_second=total, exceeded=total>limit))
    return dict(max_bits=peak, violation_events=exceeded, samples=samples)


def buckets(source, output, maximum, burst, initial_absolute=0, observed_source_startup=False):
    # Integrate the exact piecewise-constant rolling input read rate. A source
    # arrival adds bits/sec for one second; its expiry subtracts them. Credit
    # never exceeds the declared burst and starts at zero. Negative balances
    # preserve overspend debt instead of forgiving every violating AU.
    events = []
    for t,bits in source:
        events.extend(((t,0,bits),(t+1000000,0,-bits)))
    events.extend((t,1,bits) for t,bits in output)
    events.sort(key=lambda event:(event[0],event[1]))
    source_rate = 0
    source_credit = 0
    absolute_credit = initial_absolute*1000000  # exact bit-microseconds
    previous = events[0][0]
    rows = [];initial_source=0
    for t,kind,bits in events:
        elapsed = t-previous
        source_credit = min(burst*1000000, source_credit + source_rate*elapsed)
        absolute_credit = min(burst*1000000, absolute_credit + maximum*elapsed)
        previous = t
        if not kind:
            source_rate += bits
            if source_rate < 0: raise ValueError('Negative source rate')
            continue
        if not rows and observed_source_startup:
            # One source window only from messages already completed now.
            initial_source=min(burst,source_rate)
            source_credit=initial_source*1000000
        source_credit -= bits*1000000; absolute_credit -= bits*1000000
        rows.append(dict(time_us=t, bits=bits, source_rate_bps=source_rate,
                         effective_rate_bps=min(source_rate,maximum),
                         source_credit_bits=source_credit/1000000, absolute_credit_bits=absolute_credit/1000000,
                         source_exceeded=source_credit < 0, absolute_exceeded=absolute_credit < 0))
    return dict(burst_bits=burst, initial_credit_bits=0, initial_absolute_credit_bits=initial_absolute,initial_source_credit_bits=initial_source,
                source_violation_events=sum(r['source_exceeded'] for r in rows),
                absolute_violation_events=sum(r['absolute_exceeded'] for r in rows),
                max_source_debt_bits=max([0]+[-r['source_credit_bits'] for r in rows]),
                max_absolute_debt_bits=max([0]+[-r['absolute_credit_bits'] for r in rows]), rows=rows)


def audit(directory, maximum=1100000, burst=55000, deadline_ms=300, future=4):
    state = json.loads((directory/'final-state.json').read_text())
    prefix = pathlib.Path(state['log_path'])
    if not state.get('network_read',{}).get('enabled') or not str(prefix) or str(prefix)=='.':
        raise ValueError('Requires network_trace and a complete trace sink')
    files = sorted(prefix.parent.glob(prefix.name+'.*.jsonl'))
    if not files or list(prefix.parent.glob(prefix.name+'.*.active')):
        raise ValueError('Missing or unsealed trace segments')
    events = []
    for path in files:
        if path.stat().st_size > 128*1024*1024: raise ValueError('Trace segment exceeds audit bound')
        with path.open() as stream:
            for line in stream:
                event=json.loads(line)
                if event['event'] in ('frame','input_video_packet'): events.append(event)
                if len(events)>500000: raise ValueError('Audit frame bound exceeded')
    source = sorted((e for e in events if e['event']=='input_video_packet'),key=lambda e:e['sequence'])
    frames = [e for e in events if e['event']=='frame']
    if not source or [e['sequence'] for e in source] != list(range(len(source))):
        raise ValueError('Source packet trace incomplete or duplicated')
    if len(frames)!=state['packets_out'] or len({f['frame_id'] for f in frames})!=len(frames):
        raise ValueError('Output frame trace incomplete or duplicated')
    if any(state['logging'][key] for key in ('dropped_frames','dropped_status','io_errors')):
        raise ValueError('Logger loss invalidates audit')
    for f in frames:
        s=source[f['input_sequence']]
        if f['network_first_read_monotonic_us']!=s['first_read_monotonic_us'] or f['network_tag_position']!=s['tag_position']:
            raise ValueError('Frame lost source read provenance')
        if not 0<s['first_read_monotonic_us']<=s['rtmp_complete_monotonic_us']<=s['demux_finished_monotonic_us']<=f['mux_flush_done_monotonic_us']:
            raise ValueError('Invalid monotonic read/write chain')
        actual=(f['mux_flush_done_monotonic_us']-s['first_read_monotonic_us'])/1000
        if abs(actual-f['network_to_mux_ms'])>1e-6: raise ValueError('Latency boundary mismatch')
    observed_future=max(f['encoder_future_frames'] for f in frames)
    if observed_future>future or observed_future!=state['network_read']['encoder_future_max']:
        raise ValueError('Encoder future-frame bound violated/mismatched')
    latencies=[f['network_to_mux_ms'] for f in frames]
    violated=sum(v>deadline_ms for v in latencies)
    if violated!=state['network_read']['exceeded'] or state['network_to_mux']['count']!=len(frames) or abs(max(latencies)-state['network_to_mux']['max_ms'])>1e-6:
        raise ValueError('All-frame latency counters disagree with traces')
    source_bits=[(s['rtmp_complete_monotonic_us'],8*(s['bytes']+s['config_bytes'])) for s in source]
    output_bits=[(f['mux_flush_done_monotonic_us'],8*(f['encoded_bytes']+f['video_config_bytes'])) for f in frames]
    wall=rolling(output_bits,maximum)
    dts=rolling([(f['output_dts_us'],8*(f['encoded_bytes']+f['video_config_bytes'])) for f in frames],maximum)
    active=state.get('rate_control')=='constrained_crf'
    fixed_enabled=not active or bool(state.get('maxrate'))
    source_enabled=not active or bool(state.get('source_rate_limit'))
    # A declared full one-second startup allowance is not a frame cap. The
    # exact rolling 1s windows remain mandatory; unread source gets no credit.
    budget=buckets(source_bits,output_bits,maximum,burst,min(burst,maximum) if active else 0,observed_source_startup=active)
    stages=('network_assembly_ms','demux_buffer_ms','demux_to_ingress_ms','input_queue_ms','decode_ms','setup_or_dispatch_ms','filter_ms','encoder_prepare_ms','encode_ms','mux_ms')
    if any(abs(sum(f[k] for k in stages)-f['network_to_mux_ms'])>.001 for f in frames):
        raise ValueError('Latency stages do not cover the whole boundary')
    controller=None
    if active:
        if any(not 0<=f['rc_qp']<=63 or not -12<=f['rc_actual_slice_qp']<=63 or
               not f['filtered_monotonic_us']<=f['rc_decision_monotonic_us']<=f['encoder_submit_monotonic_us']
               for f in frames):raise ValueError('Invalid causal QP decision')
        encoded=iter(sorted(frames,key=lambda f:f['encoded_monotonic_us']))
        next_encoded=next(encoded,None);last_coded=None
        for f in sorted(frames,key=lambda f:f['rc_decision_monotonic_us']):
            while next_encoded is not None and next_encoded['encoded_monotonic_us']<=f['rc_decision_monotonic_us']:
                last_coded=next_encoded['output_dts_us'];next_encoded=next(encoded,None)
            if last_coded is not None and f['rc_media_clock_us']!=last_coded:
                raise ValueError('Media budget advanced without a completed coded DTS')
        if source_enabled:
            arrivals=iter(sorted(source_bits));next_arrival=next(arrivals,None)
            recent=collections.deque();available=0
            for f in sorted(frames,key=lambda f:f['rc_decision_monotonic_us']):
                observed=f['rc_decision_monotonic_us']
                while next_arrival is not None and next_arrival[0]<=observed:
                    recent.append(next_arrival);available+=next_arrival[1];next_arrival=next(arrivals,None)
                while recent and recent[0][0]<=observed-1000000:available-=recent.popleft()[1]
                expected=min(state.get('maxrate') or 100000000,f['rc_source_bps'])
                if not 0<=f['rc_source_bps']<=available or f['rc_effective_bps']!=expected:
                    raise ValueError('Source decision credited unread bytes or wrong dual minimum')
        controller=dict(mode='experimental_constrained_crf',window_ms=1000,
            requested_qp=summary([f['rc_qp'] for f in frames]),actual_slice_qp=summary([f['rc_actual_slice_qp'] for f in frames]),
            saturated_frames=sum(f['rc_saturated'] for f in frames))
    pump=None
    if state.get('decoder_pump'):
        if any(not f['decoder_submit_monotonic_us']<=f['async_ready_monotonic_us']<=f['decoded_monotonic_us'] for f in frames):
            raise ValueError('Invalid async ready / dispatch provenance')
        pump=dict(async_ready_ms=summary([f['async_ready_ms'] for f in frames]),
                  dispatch_queue_ms=summary([f['decoded_dispatch_queue_ms'] for f in frames]))
    failed=violated or (fixed_enabled and (wall['violation_events'] or dts['violation_events'] or budget['absolute_violation_events'])) or (source_enabled and budget['source_violation_events'])
    result=dict(measurement_status='complete', conformance_status='failed' if failed else 'passed',
                decoder_pump=pump,absolute_budget_enabled=fixed_enabled,source_budget_enabled=source_enabled,
                enforcement=active, controller=controller, maximum_video_bps=maximum, video_byte_scope='AVPacket payload plus codec configuration once; audio and FLV/RTMP overhead excluded',
                source_rate_scope='past 1s of completed RTMP video messages, monotonic clock; unread message bytes never credited',
                deadline_ms=deadline_ms, frames=len(frames), input_packets=len(source),
                latency_ms=summary(latencies), deadline_exceeded=violated, encoder_future_max=observed_future,
                stage_ms={k:summary([f[k] for f in frames]) for k in stages},
                groups={name:summary([f['network_to_mux_ms'] for f in frames if condition(f)]) for name,condition in (
                    ('input_i',lambda f:f['decoded_picture_type']=='I'),('first_input_i',lambda f:f['first_i']),
                    ('recurring_input_i',lambda f:f['decoded_picture_type']=='I' and not f['first_i']),
                    ('output_key',lambda f:f['output_key']))},
                output_wall_rolling_1s=wall,output_dts_rolling_1s=dts,dual_buckets=budget)
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=pathlib.Path)
    p.add_argument('--maxrate',type=int,default=1100000)
    p.add_argument('--burst-bits',type=int,default=55000)
    p.add_argument('--require-conformance',action='store_true')
    a=p.parse_args()
    if a.maxrate<=0 or a.burst_bits<0:p.error('Invalid audit budget')
    result=audit(a.directory.resolve(),a.maxrate,a.burst_bits)
    output=a.directory/'budget-audit.json'
    with output.open('x') as stream: json.dump(result,stream,indent=2,allow_nan=False);stream.write('\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('output_wall_rolling_1s','output_dts_rolling_1s','dual_buckets','stage_ms','groups')},indent=2))
    return 1 if a.require_conformance and result['conformance_status']!='passed' else 0


if __name__=='__main__':raise SystemExit(main())
