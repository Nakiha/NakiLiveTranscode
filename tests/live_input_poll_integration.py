"""Pause a real TS transport with cached video behind an unknown late PID."""
import argparse,gzip,json,pathlib,socket,struct,subprocess,tempfile,threading,time
ROOT=pathlib.Path(__file__).resolve().parents[1]

def section_fixture(original):
    """The final PMT is a CRC-valid version-31 section spanning two packets.
    No later PMT can hide loss or make a fast file reader skip its observation."""
    from live_stream_fixtures import crc_mpeg
    result=[];cut=None;changed=False
    target=max(i for i in range(0,len(original),188)
               if (((original[i+1]&31)<<8)|original[i+2])==4096 and original[i+1]&64)
    for offset in range(0,len(original),188):
        packet=bytearray(original[offset:offset+188]);pid=((packet[1]&31)<<8)|packet[2]
        if offset==target:
            control=(packet[3]>>4)&3;payload=4+(packet[4]+1 if control&2 else 0)
            start=payload+1+packet[payload];length=((packet[start+1]&15)<<8)|packet[start+2]
            section=packet[start:start+3+length];info=((section[10]&15)<<8)|section[11]
            descriptors=section[12:12+info]+bytes([0x80,240])+bytes(240)
            body=bytearray(section[:12])+descriptors+section[12+info:-4]
            body[5]=(body[5]&0xc1)|(31<<1);body[10]=0xf0|(len(descriptors)>>8);body[11]=len(descriptors)&255
            length=len(body)-3+4;body[1]=(body[1]&0xf0)|(length>>8);body[2]=length&255
            body+=crc_mpeg(body);assert 183<len(body)<=367
            assert crc_mpeg(body)==bytes(4)
            cc=packet[3]&15
            first=bytes([0x47,0x50,0,0x10|cc])+b'\0'+body[:183]
            second=(bytes([0x47,0x10,0,0x10|((cc+1)&15)])+body[183:]).ljust(188,b'\xff')
            assert len(first)==len(second)==188
            cut=sum(map(len,result))+188+73
            result.extend((first,second));changed=True;continue
        if changed and pid==4096:packet[3]=(packet[3]&0xf0)|((packet[3]+1)&15)
        result.append(packet)
    assert changed and cut is not None
    return b''.join(result),cut
def run():
    parser=argparse.ArgumentParser();parser.add_argument('--probe',required=True);parser.add_argument('--source',required=True);parser.add_argument('--baseline',action='store_true')
    parser.add_argument('--transport-fixture',default=str(ROOT/'out/bin/naki-live-transport-fixture'))
    args=parser.parse_args();run=pathlib.Path(tempfile.mkdtemp(prefix='input-poll-',dir=ROOT/'out'))
    original=pathlib.Path(args.source).read_bytes();assert len(original)%188==0
    results={}
    def probe(uri,stop=550,cancel=0,poll=1,genpts=0):
        command=[args.probe,str(uri),str(poll),str(stop),str(cancel)]+(['1'] if genpts else [])
        result=subprocess.run(command,capture_output=True,text=True,timeout=20)
        assert result.returncode==0,(uri,result.returncode,result.stderr)
        return json.loads(result.stdout)
    def network(data,kind,stop=550,cancel=0,reset=False,cut_override=None,genpts=0):
        sock=socket.socket();sock.bind(('127.0.0.1',0));sock.listen(1);sock.settimeout(10);port=sock.getsockname()[1];errors=[];connections=[]
        # Eight seconds of 1Mbps fixture followed by 73 bytes of a physical packet.
        physical=188 if kind=='tcp188' else 192 if kind=='tcp192' else 204 if kind=='tcp204' else 188
        cut=(1000000//188)*physical+73
        if kind=='http-icy':cut=123*(8192+33)+8192+1+7 # suspend within metadata, not media
        if cut_override is not None:cut=cut_override
        def send():
            try:
                conn,_=sock.accept();connections.append(1)
                with conn:
                    if kind.startswith('http'):
                        request=b''
                        while b'\r\n\r\n' not in request:request+=conn.recv(4096)
                        framing='Transfer-Encoding: chunked' if kind.startswith('http-chunk') else f'Content-Length: {len(data)}'
                        extra='Content-Encoding: gzip\r\n' if kind=='http-gzip' else 'icy-metaint: 8192\r\n' if kind=='http-icy' else ''
                        conn.sendall(f'HTTP/1.1 200 OK\r\n{framing}\r\n{extra}Connection: close\r\n\r\n'.encode())
                    if kind=='http-chunk-header':
                        conn.sendall(f'{cut:x}\r\n'.encode()+data[:cut]+b'\r\n')
                        header=f'{len(data)-cut:x}\r\n'.encode();conn.sendall(header[:2]);time.sleep(1.2)
                        conn.sendall(header[2:]+data[cut:]+b'\r\n0\r\n\r\n')
                        return
                    if kind=='http-chunk-body':conn.sendall(f'{len(data):x}\r\n'.encode())
                    conn.sendall(data[:cut]);time.sleep(1.2)
                    if reset:conn.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
                    else:
                        conn.sendall(data[cut:])
                        if kind=='http-chunk-body':conn.sendall(b'\r\n0\r\n\r\n')
            except (BrokenPipeError,ConnectionResetError):pass
            except BaseException as error:errors.append(str(error))
            finally:sock.close()
        thread=threading.Thread(target=send,daemon=True);thread.start()
        uri=f'http://127.0.0.1:{port}/live.ts' if kind.startswith('http') else f'tcp://127.0.0.1:{port}'
        result=probe(uri,stop,cancel,genpts=genpts);thread.join(timeout=3)
        assert not thread.is_alive() and not errors and len(connections)==1,(errors,connections)
        return result
    for size in (188,192,204):
        data=b''.join((b'\0'*4 if size==192 else b'')+original[i:i+188]+(b'\0'*16 if size==204 else b'') for i in range(0,len(original),188))
        source=run/f'source-{size}.ts';source.write_bytes(data)
        reference=probe(source)
        result=network(data,f'tcp{size}');results[f'tcp{size}']=result
        (run/f'tcp{size}.json').write_text(json.dumps(result,indent=2))
        assert result['streams']==reference['streams'] and result['video']==550,(size,result,reference)
        if args.baseline:
            assert result['video_before_500ms']<190 and result['max_read_ms']>700,result
        else:
            assert result['poll_available'] and result['video_before_500ms']>=190 and result['max_read_ms']<200,result
            assert result['eagain']>0,result
    if not args.baseline:
        reference=probe(run/'source-188.ts')
        section,cut=section_fixture(original);section_source=run/'source-section.ts';section_source.write_bytes(section)
        section_reference=probe(section_source,stop=0,poll=0)
        section_baseline=probe(run/'source-188.ts',stop=0)
        result=network(section,'tcp-section',stop=0,cut_override=cut);results['tcp-section']=result
        result['section_pause']={'cut_bytes':cut,'continuation_bytes_delivered':cut%188,'target_version':31,'section_ts_packets':2}
        (run/'tcp-section.json').write_text(json.dumps(result,indent=2))
        assert result['eof'] and result['streams']==section_reference['streams']==section_baseline['streams'],result
        assert result['section_versions']&(1<<31) and section_reference['section_versions']&(1<<31),result
        assert result['eagain']>0 and result['video_before_500ms']>=190 and result['max_read_ms']<200,result
        for kind in ('http','http-chunk-body','http-chunk-header','http-gzip','http-icy'):
            wire=original
            if kind=='http-gzip':wire=gzip.compress(original,compresslevel=0)
            if kind=='http-icy':
                metadata=b"StreamTitle='naki';".ljust(32,b'\0')
                wire=b''.join(original[i:i+8192]+(bytes([2])+metadata if len(original[i:i+8192])==8192 else b'') for i in range(0,len(original),8192))
            result=network(wire,kind);results[kind]=result
            (run/f'{kind}.json').write_text(json.dumps(result,indent=2))
            assert result['streams']==reference['streams'] and result['video_before_500ms']>=190,result
            assert result['max_read_ms']<200 and result['eagain']>0,result
        # Opt-in polling must not change the ordinary file demux packet ledger.
        assert probe(run/'source-188.ts',poll=0)['streams']==reference['streams']
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
            sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        source=run/'source-188.ts';sender_log=run/'srt-sender.log'
        with sender_log.open('w') as log:
            sender=subprocess.Popen([args.transport_fixture,'serve-pause',f'srt://127.0.0.1:{port}?mode=listener&latency=50000',str(source),'125000'],stdout=log,stderr=log)
            try:
                time.sleep(.15)
                result=probe(f'srt://127.0.0.1:{port}?mode=caller&latency=50000');results['srt']=result
                (run/'srt.json').write_text(json.dumps(result,indent=2))
                assert result['streams']==reference['streams'] and result['video_before_500ms']>=190,result
                assert result['max_read_ms']<200 and result['eagain']>0,result
                sender.wait(timeout=6)
            finally:
                if sender.poll() is None:sender.terminate();sender.wait(timeout=5)
        text=sender_log.read_text();assert text.count('CONNECTED_ONCE')==1 and 'SOURCE_PAUSED' in text and 'SOURCE_RESUMED' in text,text
        eof=network(original,'tcp188',stop=0);reference_eof=probe(run/'source-188.ts',stop=0);results['eof']=eof
        assert eof['eof'] and eof['streams']==reference_eof['streams'],(eof,reference_eof)
        error=network(original,'tcp188',stop=0,reset=True);results['error']=error
        assert not error['eof'] and not error['cancelled'] and error['result']<0,error
        cancel=network(original,'tcp188',stop=0,cancel=350);results['cancel']=cancel
        assert cancel['cancelled'] and not cancel['eof'] and cancel['elapsed_ms']<600,cancel
        # Exercise the public poll API with GENPTS already set. The native
        # regression separately forces its unresolved cached-PTS branch.
        genpts_eof=network(original,'tcp-genpts',stop=0,genpts=1);results['genpts-eof']=genpts_eof
        assert genpts_eof['genpts'] and genpts_eof['eof'] and genpts_eof['streams']==reference_eof['streams'],genpts_eof
        genpts_error=network(original,'tcp-genpts',stop=0,reset=True,genpts=1);results['genpts-error']=genpts_error
        assert genpts_error['genpts'] and genpts_error['result']==error['result'] and not genpts_error['eof'],genpts_error
        genpts_cancel=network(original,'tcp-genpts',stop=0,cancel=350,genpts=1);results['genpts-cancel']=genpts_cancel
        assert genpts_cancel['genpts'] and genpts_cancel['cancelled'] and not genpts_cancel['eof'] and genpts_cancel['elapsed_ms']<600,genpts_cancel
    (run/'COMPLETE.json').write_text(json.dumps({'baseline':args.baseline,'results':results},indent=2))
    print('PASS',('baseline reproduces blocked input' if args.baseline else 'cached video / partial TS / recovery / EOF / error / cancel'),run,flush=True)
if __name__=='__main__':run()
