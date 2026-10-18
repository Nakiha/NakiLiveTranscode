"""Generate a fresh late-PID fixture and run transport plus service regressions."""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import os,pathlib,subprocess,tempfile
from support.live_stream_fixtures import create
ROOT=pathlib.Path(__file__).resolve().parents[2]
def main():
    run=pathlib.Path(tempfile.mkdtemp(prefix='input-stability-fixture-',dir=ROOT/'out'))
    cache=pathlib.Path(os.environ.get('NAKI_BUILD_ROOT',pathlib.Path.home()/'.cache/naki-live-transcode-n9'))
    bins=pathlib.Path(os.environ.get('NAKI_SERVICE_BIN',ROOT/'out/bin'))
    source,_=create(run,cache/'install/bin/ffmpeg',duration=24)
    subprocess.run(['python3',str(ROOT/'tests/integration/live_input_poll_integration.py'),'--probe',str(bins/'naki-live-input-poll-test'),
                    '--transport-fixture',str(bins/'naki-live-transport-fixture'),'--source',str(source)],check=True,timeout=65)
    subprocess.run(['python3',str(ROOT/'tests/integration/live_input_service_integration.py'),'--source',str(source)],check=True,timeout=45)
if __name__=='__main__':main()
