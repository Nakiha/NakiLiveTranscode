"""Late PMT/FLV tracks without rewriting any retained video PES/tag payload."""
import pathlib
import subprocess


def crc_mpeg(data):
    value=0xffffffff
    for byte in data:
        value^=byte<<24
        for _ in range(8):value=((value<<1)^0x04c11db7 if value&0x80000000 else value<<1)&0xffffffff
    return value.to_bytes(4,"big")


def make_ts(full, late):
    source=full.read_bytes(); result=[]; first=None; phase=0; data_announced=False; data_sent=False
    for offset in range(0,len(source),188):
        packet=bytearray(source[offset:offset+188]);assert len(packet)==188 and packet[0]==0x47
        pid=((packet[1]&31)<<8)|packet[2]; control=(packet[3]>>4)&3
        if control&2 and packet[4]>=7 and packet[5]&16:
            base=(packet[6]<<25)|(packet[7]<<17)|(packet[8]<<9)|(packet[9]<<1)|(packet[10]>>7)
            if first is None:first=base
            phase=(base-first)/90000
        if pid==4096 and packet[1]&64:
            payload=4+(packet[4]+1 if control&2 else 0); start=payload+1+packet[payload]
            section=packet[start:]; length=((section[1]&15)<<8)|section[2]
            section=section[:3+length]; program_info=((section[10]&15)<<8)|section[11]
            header=bytearray(section[:12+program_info]); entries=[]; pos=len(header)
            while pos<len(section)-4:
                n=5+((section[pos+3]&15)<<8)+section[pos+4]
                entry=section[pos:pos+n]; es_pid=((entry[1]&31)<<8)|entry[2]
                if es_pid==256 or es_pid==257 and phase>=2 or es_pid==258 and phase>=4:entries.append(entry)
                pos+=n
            version=0 if phase<2 else 1 if phase<4 else 2 if phase<6 else 3
            header[5]=(header[5]&0xc1)|(version<<1)
            if phase>=6:
                entries.append(bytes([0x06,0xe1,0x03,0xf0,0])) # unknown late private data forces a codec probe
                data_announced=True
            body=header+b"".join(entries); n=len(body)-3+4
            body[1]=(body[1]&0xf0)|(n>>8);body[2]=n&255
            body+=crc_mpeg(body);assert start+len(body)<=188
            packet[start:]=body+b"\xff"*(188-start-len(body))
        if pid==257 and (phase<2 or 3<=phase<5) or pid==258 and (phase<4 or 4.7<=phase<6):
            packet=bytearray(b"\x47\x1f\xff\x10"+b"\xff"*184)
        if pid==0x1fff and data_announced and not data_sent:
            pts=(base+63000)&((1<<33)-1)
            stamp=bytes([0x21|((pts>>29)&14),(pts>>22)&255,1|((pts>>14)&254),(pts>>7)&255,1|((pts<<1)&254)])
            payload=b"naki-unknown-private-data"
            pes=b"\0\0\1\xbd"+(len(payload)+8).to_bytes(2,"big")+b"\x80\x80\x05"+stamp+payload
            adapt=183-len(pes)
            packet=bytearray(b"\x47\x41\x03\x30"+bytes([adapt,0])+b"\xff"*(adapt-1)+pes)
            assert len(packet)==188;data_sent=True
        result.append(packet)
    assert data_sent
    late.write_bytes(b"".join(result))


def tag(kind, timestamp, body):
    return (bytes([kind])+len(body).to_bytes(3,"big")+(timestamp&0xffffff).to_bytes(3,"big")+
            bytes([timestamp>>24])+b"\0\0\0"+body+(len(body)+11).to_bytes(4,"big"))


def flv_audio(body):
    if body[0]>>4==10:return 0,body[1] # legacy AAC track 0
    assert body[0]>>4==9 and body[0]&15==5,body[:8]
    return body[6],body[1]&15 # enhanced one-track AAC, track > 0


def make_flv(full, late):
    data=full.read_bytes();assert data[:3]==b"FLV"
    header=bytearray(data[:13]);header[4]=1 # initial video-only header
    result=[header];seq={}; started=set();pos=13; origin=None
    while pos<len(data):
        kind=data[pos];size=int.from_bytes(data[pos+1:pos+4],"big")
        timestamp=int.from_bytes(data[pos+4:pos+7],"big")|(data[pos+7]<<24)
        body=data[pos+11:pos+11+size];pos+=11+size+4
        if kind==18:continue # no initial metadata announcing future audio
        if kind==8:
            track,packet_type=flv_audio(body)
            if packet_type==0:seq[track]=body;continue
            phase=timestamp/1000
            if track==0 and (phase<2 or 3<=phase<5) or track==1 and (phase<4 or 4.7<=phase<6):continue
            if track not in started:
                assert track in seq
                result.append(tag(8,timestamp,seq[track]));started.add(track)
        result.append(tag(kind,timestamp,body))
    assert started=={0,1},started
    late.write_bytes(b"".join(result))


def create(run, ffmpeg):
    full=run/"full.ts"
    command=["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=256x144:rate=25",
             "-f","lavfi","-i","sine=frequency=440:sample_rate=48000",
             "-f","lavfi","-i","sine=frequency=880:sample_rate=48000","-map","0:v","-map","1:a","-map","2:a",
             "-t","8","-c:v","libx264","-preset","ultrafast","-threads","2",
             "-x264-params","bframes=3:b-adapt=0:keyint=25:scenecut=0","-c:a","aac","-ac","2","-b:a","64k",
             "-streamid","0:256","-streamid","1:257","-streamid","2:258",
             "-mpegts_pmt_start_pid","4096","-muxrate","1000000","-pcr_period","20",str(full)]
    subprocess.run(command,check=True,timeout=30)
    late_ts=run/"late.ts";make_ts(full,late_ts)
    full_flv=run/"full.flv"
    subprocess.run([str(ffmpeg),"-v","error","-i",str(full),"-map","0","-c","copy","-f","flv",str(full_flv)],
                   check=True,timeout=20)
    late_flv=run/"late.flv";make_flv(full_flv,late_flv)
    return late_ts,late_flv
