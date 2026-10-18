#!/usr/bin/env python3
"""Native RTMP origin through probe/parser buffering, without changing payloads."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0,str(_pathlib.Path(__file__).resolve().parents[1]))
import json
import subprocess
import time
from support.service_harness import BIN,new_run,port
from benchmarks.rtmp_capacity import wait_listener

def main():
    run=new_run('read-origin-')
    source=run/'input.flv'
    subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=320x180:rate=60',
                    '-frames:v','60','-c:v','libx264','-threads','1','-g','120','-bf','3','-qp','12',
                    '-output_ts_offset','1','-f','flv',str(source)],check=True)
    results=[]
    for enabled in (0,1):
        directory=run/str(enabled);directory.mkdir()
        source_port=port();url=f'rtmp://127.0.0.1:{source_port}/live/source'
        with (directory/'sender.log').open('w') as log:
            sender=subprocess.Popen([str(BIN/'naki-live-transport-fixture'),'serve-hold',url,str(source),'flv','20'],stdout=log,stderr=log)
            try:
                wait_listener(sender,source_port)
                result=subprocess.run([str(BIN/'naki-read-origin-probe'),url,str(enabled)],capture_output=True,text=True,timeout=15)
                (directory/'probe.json').write_text(result.stdout);(directory/'probe.stderr').write_text(result.stderr)
                assert result.returncode==0,(result.returncode,result.stderr)
                packets=json.loads(result.stdout);assert len(packets)==16
                results.append(packets)
                if enabled:
                    assert packets[0]['first_read_us']<=packets[0]['probed_us']
                    assert packets[0]['returned_us']-packets[0]['first_read_us']>=200000
                    assert all(0<p['first_read_us']<=p['complete_us']<=p['returned_us'] for p in packets)
                else:assert all(p['first_read_us']==0 for p in packets)
            finally:
                sender.terminate();sender.wait(timeout=5)
    assert [[(p['pts'],p['dts'],p['bytes'],p['checksum']) for p in rows] for rows in results][0]==[
        (p['pts'],p['dts'],p['bytes'],p['checksum']) for p in results[1]]
    (run/'COMPLETE.json').write_text(json.dumps(dict(status='passed',cases=['disabled_payload_equivalence','chunked_avc','parser_complete_au','probe_buffer_origin','delayed_consumer']))+'\n')
    print('PASS read origin:',run)

if __name__=='__main__':main()
