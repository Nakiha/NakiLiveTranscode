#!/usr/bin/env python3
"""Opt-in cumulative worker CPU ledger; nested phases are never added twice."""
import argparse
import json
from pathlib import Path

def audit(directory):
    directory=Path(directory)
    state=json.loads((directory/'final-state.json').read_text())
    prefix=Path(state['log_path'])
    assert not list(prefix.parent.glob(prefix.name+'.*.active')), 'Unsealed trace'
    events=[json.loads(line) for path in sorted(prefix.parent.glob(prefix.name+'.*.jsonl')) for line in path.read_text().splitlines()]
    frames=[e['encoder_internal'] for e in events if e.get('event')=='frame']
    assert frames and all(e['version']==4 and e['pool_ledger_enabled']==1 for e in frames)
    accepted=sorted((e for e in frames if e['pool_ledger_valid']),key=lambda e:e['ledger_snapshot_end_us'])
    assert len(accepted)>=len(frames)*.90 and len(accepted)>=10
    assert len({e['ledger_caller_tid'] for e in accepted})==1 and accepted[0]['ledger_caller_tid']>0
    categories=('pool_scan_cpu_us','pool_yield_cpu_us','pool_lock_cpu_us','pool_tasks_cpu_us')
    tasks=('pool_mctf_me_cpu_us','pool_mctf_filter_cpu_us','pool_ctu_cpu_us','pool_other_cpu_us')
    names=('pool_total_cpu_us',)+categories+tasks+('ledger_process_cpu_us','ledger_caller_cpu_us','pool_scan_calls','pool_ready_calls','pool_ready_failed','pool_empty_scans','pool_task_calls')
    for e in accepted:
        assert all(e[n]>=0 for n in names)
        assert sum(e[n] for n in tasks)==e['pool_tasks_cpu_us']
        assert sum(e[n] for n in categories)<=e['pool_total_cpu_us']
    for a,b in zip(accepted,accepted[1:]):
        assert all(b[n]>=a[n] for n in names), 'Cumulative counter rewound'
    first,last=accepted[0],accepted[-1]
    delta={n:last[n]-first[n] for n in names}
    assert sum(delta[n] for n in tasks)==delta['pool_tasks_cpu_us']
    pool_rest=delta['pool_total_cpu_us']-sum(delta[n] for n in categories)
    assert pool_rest>=0
    process_rest=delta['ledger_process_cpu_us']-delta['ledger_caller_cpu_us']-delta['pool_total_cpu_us']
    assert delta['ledger_process_cpu_us']>0
    components={n:delta[n] for n in categories}
    components['pool_other_cpu']=pool_rest
    components['encoder_caller_cpu']=delta['ledger_caller_cpu_us']
    # This is a signed accounting residual, not a synchronized measurement
    # of decoder/logger/network CPU. Pool slots were last published at
    # different times before each completed-frame snapshot.
    components['process_unattributed_plus_window_skew_cpu']=process_rest
    return dict(status='passed',frames=len(frames),valid_snapshots=len(accepted),
                caller_tid=first['ledger_caller_tid'],
                boundary='First to last valid completed-frame snapshot; excludes startup/close. Process/caller clocks are sampled after per-thread pool publications, so the CPU windows are not synchronized.',
                wall_span_us=last['ledger_snapshot_end_us']-first['ledger_snapshot_end_us'],
                components_us=components,
                task_subsets_us={n:delta[n] for n in tasks},counters=delta,
                nominal_process_share_percent={n:v/delta['ledger_process_cpu_us']*100 for n,v in components.items()},
                alignment=dict(pool_publication='Each worker last task boundary or every 64 scans before the frame snapshot.',
                               publication_lag_bound_us=None,
                               process_clock='Sampled after the pool snapshot.',
                               precision='No common CPU-clock window or bounded publication lag; signed process residual includes other threads and first/last publication skew. Algebraic summation is not evidence of precise attribution.'),
                caveat='Pool scan/yield/tasks are disjoint within each worker publication; MCTF/CTU subsets are already inside tasks. Frame phase timers overlap and are never added again. Diagnostic overhead is included; this is not formal performance.')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    result=audit(args.directory)
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
