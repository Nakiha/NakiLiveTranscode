"""Missing video/parameters, independent budgets, and mid-probe error injection."""
import re


def run(result, command, binary, observe=False):
    audio=result/"budget-audio.ts"
    command(["ffmpeg","-v","error","-f","lavfi","-i","sine=frequency=440:sample_rate=48000",
             "-t","8","-c:a","aac","-b:a","64k","-f","mpegts",audio])
    raw=result/"budget-valid.h264"
    command(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=320x180:rate=25",
             "-t","8","-c:v","libx264","-preset","ultrafast","-threads","1","-bf","0","-f","h264",raw])
    data=raw.read_bytes();starts=list(re.finditer(b"\x00\x00(?:\x00)?\x01",data));parts=[]
    for index,marker in enumerate(starts):
        end=starts[index+1].start() if index+1<len(starts) else len(data)
        if data[marker.end()]&31 not in (7,8):parts.append(data[marker.start():end])
    broken=result/"budget-no-parameters.h264";broken.write_bytes(b"".join(parts))
    valid_ts=result/"budget-valid.ts"
    command(["ffmpeg","-v","error","-f","lavfi","-i","testsrc2=size=320x180:rate=25",
             "-t","8","-c:v","libx264","-preset","ultrafast","-threads","1","-bf","0","-f","mpegts",valid_ts])
    data=bytearray(valid_ts.read_bytes())
    for marker in re.finditer(b"\x00\x00(?:\x00)?\x01",data):
        if data[marker.end()]&31 in (7,8):data[marker.end()]=(data[marker.end()]&0xe0)|12
    broken_ts=result/"budget-no-parameters.ts";broken_ts.write_bytes(data)
    cases=[*( ("audio-"+mode,audio,"mpegts",mode) for mode in ("media","bytes","eio","cancel")),
           ("missing-parameters-media",broken_ts,"mpegts","media"),
           ("missing-parameters-bytes",broken,"h264","bytes"),
           ("missing-parameters-wall",broken,"h264","wall"),
           ("video-mid-eio",raw,"h264","eio"),("video-mid-cancel",raw,"h264","cancel")]
    observations=[]
    for name,path,fmt,mode in cases:
        value=command([binary,path,fmt,mode],capture=True)
        observations.append({"case":name,**value})
        if observe:continue
        expected=value["eio"] if mode=="eio" else value["exit"] if mode=="cancel" else (
            value["bytes_error"] if mode=="bytes" else value["timeout"])
        assert value["ret"]==expected,(name,value)
        assert 0<value["probe_reads"] and value["total_bytes"]<value["file_bytes"],(name,value)
        if mode=="media":assert value["total_bytes"]<value["file_bytes"]//2,(name,value)
        if mode in ("eio","cancel"):assert value["probe_reads"]==8,(name,value)
        if mode=="wall":assert value["probe_ms"]<1000,(name,value)
    return observations
