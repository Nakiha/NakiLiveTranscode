"""Cached video must reach real outputs while an unknown late PID is starved."""
import argparse,json,os,pathlib,socket,subprocess,tempfile,threading,time,urllib.error,urllib.request
ROOT=pathlib.Path(__file__).resolve().parents[1]
BIN=pathlib.Path(os.environ.get('NAKI_SERVICE_BIN',ROOT/'out/bin'))

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--source',required=True);parser.add_argument('--baseline',action='store_true');args=parser.parse_args()
    run=pathlib.Path(tempfile.mkdtemp(prefix='input-service-',dir=ROOT/'out'));data=pathlib.Path(args.source).read_bytes()
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    base=f'http://127.0.0.1:{port}';server=None;logs=[];results={}
    def api(path='/v1/job',body=None,code=200):
        req=urllib.request.Request(base+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
        try:response=urllib.request.urlopen(req,timeout=3)
        except urllib.error.HTTPError as error:response=error
        with response:
            result=json.load(response);assert response.status==code,(response.status,result);return result
    def wait(fn,timeout=8):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            result=fn()
            if result:return result
            time.sleep(.02)
        raise AssertionError('condition timed out')
    def start_server():
        nonlocal server
        log=(run/f'server-{len(logs)}.log').open('w');logs.append(log)
        server=subprocess.Popen([str(BIN/'naki-live'),'--port',str(port),'--run-dir',str(run/'logs')],stdout=log,stderr=log)
        def healthy():
            assert server.poll() is None
            try:return api('/health')
            except OSError:return False
        wait(healthy)
    def finished(expected='completed'):
        state=wait(lambda:(s if (s:=api())['state'] in ('completed','completed_partial','stopped','failed') else None))
        assert state['state']==expected,state;return state
    try:
        start_server()
        for mode in (('single',) if args.baseline else ('single','recover-multi','cancel')):
            listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(10)
            input_port=listener.getsockname()[1];paused=threading.Event();resume=threading.Event();connections=[];errors=[]
            def send():
                try:
                    conn,_=listener.accept();connections.append(1)
                    with conn:
                        cut=(1000000//188)*188+73;conn.sendall(data[:cut]);paused.set()
                        assert resume.wait(8),'source resume timed out'
                        conn.sendall(data[cut:])
                except (BrokenPipeError,ConnectionResetError):pass
                except BaseException as error:errors.append(repr(error))
                finally:listener.close()
            thread=threading.Thread(target=send,daemon=True);thread.start()
            config={'input':f'tcp://127.0.0.1:{input_port}','output':str(run/f'{mode}.ts'),'output_format':'mpegts',
                    'encoder_preset':'ultrafast','pace':False,'frame_log':True,'mux_wait_ms':50,'timestamps':{'mode':'source_all'}}
            if mode=='recover-multi':
                config.pop('output');config.pop('output_format');config.pop('encoder_preset')
                config['outputs']=[{'id':f'branch-{n}','output':str(run/f'{mode}-{n}.ts'),'output_format':'mpegts','encoder_preset':'ultrafast'} for n in range(2)]
            api(body=config,code=202)
            try:
                assert paused.wait(4),'source never reached the late PID pause'
                # No source bytes arrive in this observation window. Cached
                # video after the unknown PID must nevertheless be delivered.
                time.sleep(.75);state=api();results[mode+'-paused']=state
                (run/f'{mode}-paused.json').write_text(json.dumps(state,indent=2))
                if args.baseline:
                    assert state['frames_decoded']<180,state
                else:
                    assert state['frames_decoded']>=180,state
                    outputs=state.get('outputs') or [state]
                    assert all(s['packets_out']>=180 for s in outputs),state
                if mode=='cancel':
                    began=time.monotonic();api('/v1/job/stop',body={},code=202);state=finished('stopped')
                    assert time.monotonic()-began<1.0,state
                else:
                    if mode=='recover-multi':
                        worker=state['supervisor']['worker_pid'];job=state['job_id'];server.kill();server.wait(timeout=5);start_server()
                        state=wait(lambda:(s if (s:=api()).get('job_id')==job and s.get('supervisor',{}).get('recovered') else None),timeout=4)
                        assert state['supervisor']['worker_pid']==worker and state['state']=='running',state
                        results['recovered-paused']=state
                    resume.set();state=finished()
                    assert state['frames_decoded']==600,state
                    assert all(s['packets_out']==600 for s in (state.get('outputs') or [state])),state
                results[mode+'-terminal']=state
            finally:
                resume.set();thread.join(timeout=4)
                assert not thread.is_alive() and not errors and len(connections)==1,(errors,connections)
        (run/'COMPLETE.json').write_text(json.dumps({'baseline':args.baseline,'results':results},indent=2))
        print('PASS',('baseline cached-video starvation' if args.baseline else 'cached video reaches outputs / same worker recovery / cancel'),run,flush=True)
    finally:
        if server and server.poll() is None:server.terminate();server.wait(timeout=10)
        for log in logs:log.close()
if __name__=='__main__':main()
