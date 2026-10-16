"""Validate decoded display-order VVC frames, rather than trusting encoder options."""
import json
import subprocess


def decode_frames(probe, path):
    result=subprocess.run([str(probe),"-v","error","-threads","2","-select_streams","v:0","-show_frames",
                           "-show_entries","frame=pict_type,key_frame,pts_time","-of","json",str(path)],
                          capture_output=True,text=True,check=True,timeout=45)
    return json.loads(result.stdout)["frames"]


def check_structure(decoded, period):
    keys=[i for i,f in enumerate(decoded) if f["key_frame"]]
    assert keys==list(range(0,len(decoded),period)),keys
    types="".join(f["pict_type"] for f in decoded)
    assert all((typ=="I")==bool(f["key_frame"]) for typ,f in zip(types,decoded)),types
    assert set(types)<=set("IPB") and "BBBB" not in types,types
    if len(types)>=9:
        assert types.startswith("IBBBP") and "PBBBP" in types,types
    anchors=[i for i,t in enumerate(types) if t!="B"]
    for left,right in zip(anchors,anchors[1:]):
        assert right-left<=4,(left,right,types)
        # A partial group may terminate at an intra boundary or end of stream.
        if types[left]==types[right]=="P" and right%period!=period-1 and right!=len(types)-1:
            assert right-left==4,(left,right,types)
    times=[float(f["pts_time"]) for f in decoded]
    intervals=[]
    if len(times)>1:
        tick=times[1]-times[0]
        assert tick>0
        assert all(abs((right-left)-tick)<.00001 for left,right in zip(times,times[1:])),times
        intervals=[times[right]-times[left] for left,right in zip(keys,keys[1:])]
        assert all(abs(interval-period*tick)<.002 for interval in intervals),intervals
    return {"key_frame_indices":keys,"key_period_seconds":intervals,"display_types":types}


def check_pts_structure(decoded, source_times, seconds=2):
    """First available frame at each occupied grid boundary; no drift after gaps."""
    assert len(decoded)==len(source_times) and source_times
    assert all(a<b for a,b in zip(source_times,source_times[1:])),source_times
    slots=[int((t-source_times[0]+1e-7)//seconds) for t in source_times]
    expected=[0]+[i for i in range(1,len(slots)) if slots[i]>slots[i-1]]
    keys=[i for i,f in enumerate(decoded) if f["key_frame"]]
    assert keys==expected,(keys,expected)
    types="".join(f["pict_type"] for f in decoded)
    assert all((t=="I")==bool(f["key_frame"]) for t,f in zip(types,decoded)),types
    # Full groups end in P; only the final group before a key/EOF may be short.
    for begin,end in zip(keys,keys[1:]+[len(decoded)]):
        remaining=end-begin-1
        cursor=begin+1
        while remaining:
            size=min(4,remaining)
            assert types[cursor:cursor+size]=="B"*(size-1)+"P",(cursor,size,types)
            cursor+=size; remaining-=size
    times=[float(f["pts_time"]) for f in decoded]
    shift=times[0]-source_times[0]
    assert all(abs(t-source-shift)<.000025 for t,source in zip(times,source_times)),(times,source_times)
    return {"key_frame_indices":keys,"key_period_seconds":[times[b]-times[a] for a,b in zip(keys,keys[1:])],
            "key_target_offsets_seconds":[slots[i]*seconds for i in keys],
            "key_lateness_seconds":[source_times[i]-source_times[0]-slots[i]*seconds for i in keys],
            "display_types":types}
