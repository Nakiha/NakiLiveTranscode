#!/usr/bin/env python3
"""Hold candidate preparation across a real decoded-size generation change."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0,str(_pathlib.Path(__file__).resolve().parents[1]))
import http.server
import json
from pathlib import Path
import tempfile
import threading
import uuid
from support.images import png
from integration.rendition_process_integration import ROOT,ORACLE_FFMPEG,Harness,command,wait,require,frame_rows,decoded_hashes


def run_case(run,source,frames,mode):
    mark=run/'mark.png';png(mark);payload=mark.read_bytes();requested=threading.Event();release=threading.Event()
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            requested.set();release.wait(10);self.send_response(200);self.send_header('Content-Length',str(len(payload)));self.end_headers();
            try:self.wfile.write(payload)
            except (BrokenPipeError,ConnectionResetError):pass
        def log_message(self,*args):pass
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);server.daemon_threads=True;thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();h=Harness(run);report={}
    try:
        h.start();base=dict(id='base',output=str(run/'base.mp4'),output_format='mp4',width=160,height=96,encoder_preset='ultrafast',audio_copy=False)
        cfg=dict(input=str(source),outputs=[base],rendition_backend='process',pace=True,max_threads=2,min_threads=2,decoder_auto_threads=False,frame_log=True,timestamps={'mode':'source_all'})
        h.api(body=cfg,expected=202);initial=wait(lambda:(s if (s:=h.api()).get('frames_decoded',0)>=10 and s.get('packets_out',0)>=8 else None));job=initial['job_id'];pid=initial['outputs'][0]['rendition_pid'];generation=initial['source_format_generation']
        candidate=dict(base,id='candidate',output=str(run/'candidate.mp4'),watermark=dict(enabled=True,resource=dict(uri=f'http://127.0.0.1:{server.server_port}/mark.png',load_timeout_ms=10000),startup=dict(required=True,timeout_ms=10000)))
        body=dict(operation_id=uuid.uuid4().hex,action='add_rendition',target=dict(component='rendition',output_id='candidate'),expected_revision=initial['topology_revision'],spec=candidate)
        h.api(f'/v1/jobs/{job}/updates',body,202);require(requested.wait(4),'candidate resource load was not held')
        if mode=='format':
            changed=wait(lambda:(s if (s:=h.api()).get('frames_decoded',0)>=55 and s['source_format_generation']>generation else None),5);release.set()
        elif mode=='stop':
            h.api(f'/v1/jobs/{job}/stop',dict(operation_id=uuid.uuid4().hex),202);changed=h.api()
        else:
            changed=wait(lambda:(s if (s:=h.api()).get('frames_decoded',0)==frames else None),10)
        result=wait(lambda:(op if (op:=h.api(f'/v1/jobs/{job}/operations/{body["operation_id"]}'))['state'] in ('failed','cancelled','applied') else None),10)
        if mode=='format':require(result['state']=='failed' and 'source_format_generation' in result.get('reason',''),result)
        else:require(result['state']=='cancelled' and result['applied_checkpoint'] is None and result['delivery']['first_written'] is None,result)
        release.set()
        final=wait(lambda:(s if (s:=h.api())['state'] in ('completed','completed_partial','failed','stopped') else None),15)
        require(final['state']==('stopped' if mode=='stop' else 'completed') and (final['frames_decoded']<frames if mode=='stop' else final['frames_decoded']==frames) and final['shared_pool_occupied_slots']==0,final)
        require(len(final['outputs'])==1 and final['outputs'][0]['output_id']=='base','failed candidate remained in actual topology')
        observed=next(x for x in changed['outputs'] if x['output_id']=='base')
        require(observed['rendition_pid'] in (0,pid) and observed['rendition_generation']==initial['outputs'][0]['rendition_generation'],'format/terminal race replaced healthy encoder')
        if mode!='stop':
            rows=frame_rows(final['outputs'][0]['log_path']);require([r['frame_id'] for r in rows]==list(range(1,frames+1)),'format transition source sequence')
            require(len(decoded_hashes(base['output']))==frames,'independent size-transition output decode')
        report.update(passed=True,initial=initial,changed=changed,candidate_operation=result,final=final,expected_frames=frames)
    finally:
        release.set();h.close();server.shutdown();server.server_close();thread.join(timeout=3);(run/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    root=Path(tempfile.mkdtemp(prefix='rendition-races-',dir=ROOT/'out'));dynamic=root/'dynamic.ts';constant=root/'constant.ts'
    for index,(size,count,offset) in enumerate([('160x96',50,0),('192x112',100,2.08)]):
        command([ORACLE_FFMPEG,'-v','error','-f','lavfi','-i',f'testsrc2=size={size}:rate=25','-frames:v',count,'-c:v','libx264','-preset','ultrafast','-threads','2','-g','25','-bf','3','-output_ts_offset',offset,'-f','mpegts',root/f'segment{index}.ts'])
    dynamic.write_bytes((root/'segment0.ts').read_bytes()+(root/'segment1.ts').read_bytes())
    decoded=json.loads(command(['ffprobe','-v','error','-select_streams','v:0','-show_frames','-show_entries','frame=width,height,pts','-of','json',dynamic]))['frames']
    require([(f['width'],f['height']) for f in decoded]==[(160,96)]*50+[(192,112)]*100,'format fixture is not the fixed independent 150-frame oracle')
    require([int(f['pts']) for f in decoded]==list(range(int(decoded[0]['pts']),int(decoded[0]['pts'])+150*3600,3600)),'format fixture must have one continuous display timeline, including B-frame mux timestamp shift')
    command([ORACLE_FFMPEG,'-v','error','-f','lavfi','-i','testsrc2=size=160x96:rate=25','-frames:v',75,'-c:v','libx264','-preset','ultrafast','-threads','2','-g','25','-bf','3','-f','mpegts',constant])
    report={}
    try:
        for mode in ['format','stop','eof']:
            directory=root/mode;directory.mkdir();report[mode]=run_case(directory,dynamic if mode=='format' else constant,150 if mode=='format' else 75,mode)
        report['passed']=True
    finally:(root/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(run=str(root),passed=True)))


if __name__=='__main__':main()
