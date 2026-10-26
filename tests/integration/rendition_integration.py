#!/usr/bin/env python3
"""The single catalog gate for issue #4; every child suite must exit zero."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--phase',choices=['all','functional','metrics'],default='all');parser.add_argument('--sanitizer',action='store_true');args=parser.parse_args()
    programs=[('rendition_process_integration.py',['--phase',phase]) for phase in ['smoke','thread','lifecycle','faults']]
    programs += [(name,[]) for name in ['rendition_recovery.py','rendition_destinations.py','rendition_audio_sei.py','rendition_network_restart.py','rendition_lifetime.py','rendition_format_race.py','rendition_churn.py','rendition_matrix.py']]
    programs += [('rendition_matrix.py',['--advanced']),('rendition_matrix.py',['--sustain'])]
    programs += [('rendition_regressions.py',[])]
    if args.phase=='functional':programs=[(name,options) for name,options in programs if name!='rendition_matrix.py']
    elif args.phase=='metrics':programs=[(name,options) for name,options in programs if name=='rendition_matrix.py']
    if args.sanitizer:
        print('Intentional native fault assertions and performance are run on the normal binary. Holder guard prohibits LSan fork: process tests disable leak tracing; native quick units retain LSan. ASan/UBSan remain active.',flush=True)
        os.environ['ASAN_OPTIONS']=os.environ.get('ASAN_OPTIONS','halt_on_error=1')+':detect_leaks=0:quarantine_size_mb=16:thread_local_quarantine_size_kb=64'
        programs=[(name,options) for name,options in programs if options!=['--phase','faults'] and name!='rendition_matrix.py']
    for name,args in programs:
        print('RUN',name,*args,flush=True)
        subprocess.run([sys.executable,str(ROOT/'tests/integration'/name),*args],env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1'),check=True,timeout=600)
    print('PASS complete rendition gate',flush=True)

if __name__=='__main__':main()
