"""Independent media probes, shared without importing a test suite."""
import json
import subprocess
from .service_harness import BIN, CACHE, PROBE

def probe(path):
    result = subprocess.run([str(PROBE), "-v", "error", "-threads", "2", "-select_streams", "v:0",
                             "-count_frames", "-show_packets", "-show_entries",
                             "stream=codec_name,pix_fmt,width,height,nb_read_frames:packet=pts_time,dts_time",
                             "-of", "json", str(path)], check=True, capture_output=True, text=True, timeout=40)
    return json.loads(result.stdout)

def inspect(path):
    result=subprocess.run([str(BIN/"naki-live-transport-fixture"),"inspect",str(path)],
                          capture_output=True,text=True,check=True,timeout=40)
    return json.loads(result.stdout)["streams"]
