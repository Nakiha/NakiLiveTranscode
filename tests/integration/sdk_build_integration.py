#!/usr/bin/env python3
"""Exercise the real default build/seal/skip entry points using tiny real DSOs.

No media libraries are rebuilt here. Each fixture has its own source/cache/out
tree and runs the unchanged project build scripts with GCC/CMake/Make.
"""
import pathlib as _pathlib
import sys as _sys
_sys.path.insert(0, str(_pathlib.Path(__file__).resolve().parents[1]))
import argparse
import json
import os
import pathlib
import signal
import shutil
import subprocess
import tempfile
import textwrap
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]


def run(expect_stale=False,identity_script=None,expect_stale_transitive=False):
    result = pathlib.Path(tempfile.mkdtemp(prefix="sdk-build-", dir=ROOT / "out"))
    root = result / "source"
    scripts = root / "scripts"
    scripts.mkdir(parents=True)
    for name in ("build-service.sh", "build-ffmpeg.sh", "build-vvenc.sh", "sdk-identity.py", "sdk-expected.py", "archive-symbols.py"):
        shutil.copy2(ROOT / "scripts" / name, scripts / name)
    if identity_script:shutil.copy2(identity_script,scripts/"sdk-identity.py")
    shutil.copy2(ROOT / "Dockerfile.ffmpeg", root / "Dockerfile.ffmpeg")
    ffmpeg = root / "experiments/decoder-20261016/ffmpeg-7.0.3"
    (ffmpeg / "libavformat").mkdir(parents=True)
    source = ffmpeg / "libavformat/demux.c"
    source.write_text("#include <vvenc/marker.h>\nint format_marker(void) { return 1+VVENC_WRAPPER_MARKER; }\n")
    ffmpeg_broken = ffmpeg / "libavformat/broken.c"
    ffmpeg_broken.write_text("int format_aux(void) { return 0; }\n")
    (ffmpeg / "marker.h").write_text("int format_marker(void);\n")
    configure = ffmpeg / "configure"
    configure.write_text(textwrap.dedent('''\
        #!/usr/bin/env python3
        import os, pathlib, signal, sys
        source = pathlib.Path(__file__).resolve().parent
        prefix = next(x.split('=', 1)[1] for x in sys.argv[1:] if x.startswith('--prefix='))
        configuration_marker = 0
        pathlib.Path('ffbuild').mkdir(exist_ok=True)
        pathlib.Path('config.h').write_text(f'#define FIXTURE_HEADER_MARKER {configuration_marker}\\n')
        pathlib.Path('ffbuild/config.mak').write_text(f'FIXTURE_MAKE_MARKER := {configuration_marker}\\n')
        pathlib.Path('config.c').write_text('#include "config.h"\\n'
            'int config_marker(void) { return FIXTURE_HEADER_MARKER + FIXTURE_MAKE_MARKER; }\\n')
        pathlib.Path('Makefile').write_text(
            'source := ' + str(source) + '\\nprefix := ' + prefix + '\\n'
            'include ffbuild/config.mak\\n'
            'all: libavformat.so\\n'
            'demux.o: $(source)/libavformat/demux.c\\n'
            '\\t$(CC) -fPIC -O0 -g -I$(prefix)/include -c $< -o $@\\n'
            'broken.o: $(source)/libavformat/broken.c demux.o\\n'
            '\\t$(CC) -fPIC -O0 -g -c $< -o $@\\n'
            'config.o: config.c config.h ffbuild/config.mak\\n'
            '\\t$(CC) -fPIC -O0 -g -DFIXTURE_MAKE_MARKER=$(FIXTURE_MAKE_MARKER) -c $< -o $@\\n'
            'libavformat.so: demux.o broken.o config.o\\n'
            '\\t$(CC) -shared -Wl,--build-id=sha1 -o $@ $^\\n'
            'install: all\\n'
            '\\tmkdir -p "$(prefix)/lib" "$(prefix)/include"\\n'
            '\\tcp libavformat.so "$(prefix)/lib/"\\n'
            '\\tcp "$(source)/marker.h" "$(prefix)/include/"\\n'
            '\\tif test -n "$$SDK_TEST_MUTATE_SOURCE"; then printf "/* changed during install */\\\\n" >> "$$SDK_TEST_MUTATE_SOURCE"; fi\\n'
            'clean:\\n\\trm -f demux.o broken.o config.o libavformat.so\\n')
        with pathlib.Path('configure-calls.jsonl').open('a') as log:
            log.write(str(configuration_marker) + '\\n')
        mode = os.environ.get('SDK_TEST_CONFIGURE_MODE')
        if mode == 'fail':
            sys.exit(19)
        if mode == 'pause':
            pathlib.Path(os.environ['SDK_TEST_BARRIER']).write_text('configuration rewritten\\n')
            while True:
                signal.pause()
        '''))
    configure.chmod(0o755)
    vvenc = root / "third_party/vvenc"
    vvenc.mkdir(parents=True)
    vvenc_source = vvenc / "marker.c"
    vvenc_source.write_text("int vvenc_marker(void) { return 11; }\n")
    vvenc_broken = vvenc / "broken.c"
    vvenc_broken.write_text("int vvenc_aux(void) { return 0; }\n")
    # Exact whole-second times survive DrvFS/NTFS timestamp precision too.
    for path in (source, vvenc_source, ffmpeg_broken, vvenc_broken, configure):
        os.utime(path, ns=(1700000000000000000, 1700000000000000000))
    vvenc_header = vvenc / "marker.h"
    vvenc_header.write_text("int vvenc_marker(void); /* 11 */\n#define VVENC_WRAPPER_MARKER 0\n")
    (vvenc / "CMakeLists.txt").write_text(textwrap.dedent('''\
        cmake_minimum_required(VERSION 3.16)
        project(VVenCCacheFixture LANGUAGES C)
        add_library(vvenc SHARED marker.c broken.c)
        set_source_files_properties(broken.c PROPERTIES OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/CMakeFiles/vvenc.dir/marker.c.o")
        set_target_properties(vvenc PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
        install(TARGETS vvenc LIBRARY DESTINATION lib)
        install(FILES marker.h DESTINATION include/vvenc)
        '''))
    yyjson = root / "third_party/yyjson/src"
    yyjson.mkdir(parents=True)
    (yyjson / "yyjson.c").write_text("/* source-existence guard only */\n")
    (root / "main.c").write_text('#include <stdio.h>\nint format_marker(void); int config_marker(void); int vvenc_marker(void);\n'
                                 'int main(void) { printf("%d %d\\n", format_marker()+config_marker(), vvenc_marker()); return 0; }\n')
    (root / "CMakeLists.txt").write_text(textwrap.dedent('''\
        cmake_minimum_required(VERSION 3.16)
        project(SDKConsumerFixture LANGUAGES C)
        add_executable(naki-cache-marker main.c)
        target_link_directories(naki-cache-marker PRIVATE "${NAKI_FFMPEG_PREFIX}/lib")
        target_link_libraries(naki-cache-marker PRIVATE avformat vvenc)
        set_target_properties(naki-cache-marker PROPERTIES BUILD_RPATH "${NAKI_FFMPEG_PREFIX}/lib")
        '''))
    cache = result / "cache"
    # Pause while completing the second object: the first B object has already
    # been accepted by Make. Otherwise Make deletes the interrupted target on
    # SIGTERM, so the fixture would lose its partial-cache witness.
    compiler = result / "compiler.py"
    compiler.write_text(textwrap.dedent('''\
        #!/usr/bin/python3
        import os, pathlib, signal, subprocess, sys
        code = subprocess.call(['gcc', *sys.argv[1:]])
        # Force the install timestamp collision seen on the fast Linux CI
        # runner, without sleeping or changing the rebuilt DSO's bytes.
        installed = os.environ.get('SDK_TEST_ALIGN_INSTALL_TIMES')
        if code == 0 and installed and '-o' in sys.argv[1:]:
            output = pathlib.Path(sys.argv[sys.argv.index('-o') + 1])
            if output.name == 'libvvenc.so' and pathlib.Path(installed).exists():
                stamp = output.stat()
                os.utime(installed, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        target = os.environ.get('SDK_TEST_TARGET')
        if code == 0 and target and target in sys.argv[1:]:
            pathlib.Path(os.environ['SDK_TEST_BARRIER']).write_text('object complete\\n')
            while True:
                signal.pause()
        sys.exit(code)
        '''))
    compiler.chmod(0o755)
    environment = dict(os.environ, NAKI_BUILD_ROOT=str(cache), FFMPEG_LIVE_BUILD_JOBS="1",
                       NAKI_SKIP_FFMPEG="0", NAKI_FFMPEG_ONLY="0", NAKI_MEDIA_HOOK_SOURCE="",
                       CC=str(compiler), CMAKE_GENERATOR="Unix Makefiles", CMAKE_INSTALL_ALWAYS="0",
                       SDK_TEST_ALIGN_INSTALL_TIMES=str(cache / "install/lib/libvvenc.so"))
    environment.pop("NAKI_VVENC_SYMBOLS", None)
    environment.pop("LD_LIBRARY_PATH", None)
    # This fixture builds its own miniature application, never the caller's bin/cache.
    for name in ("NAKI_SERVICE_BIN", "NAKI_SERVICE_BUILD_DIR", "NAKI_SANITIZERS", "NAKI_BUILD_TYPE"):
        environment.pop(name, None)
    for name in ("NAKI_EXPECTED_SDK_MANIFEST", "NAKI_EXPECTED_DEPENDENCY_KEY"):
        environment.pop(name, None)
    for name in ("SDK_TEST_TARGET", "SDK_TEST_BARRIER", "SDK_TEST_MUTATE_SOURCE", "SDK_TEST_CONFIGURE_MODE"):
        environment.pop(name, None)

    def build(name, skip=False, extra=None, fail=False, interrupt=None):
        env = dict(environment, NAKI_SKIP_FFMPEG=str(int(skip)), **(extra or {}))
        with (result / (name + ".log")).open("w") as log:
            process = subprocess.Popen(["bash", str(scripts / "build-service.sh")], env=env,
                                       stdout=log, stderr=log, start_new_session=True)
            try:
                if interrupt:
                    barrier = pathlib.Path(env["SDK_TEST_BARRIER"])
                    deadline = time.monotonic() + 45
                    while not barrier.exists():
                        assert process.poll() is None, (name, "exited before partial object barrier")
                        assert time.monotonic() < deadline, (name, "partial object barrier timed out")
                        time.sleep(0.02)
                    os.killpg(process.pid, interrupt)
                code = process.wait(timeout=90)
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
        if fail or interrupt:
            assert code != 0, (name, "unexpected success")
            return {"exit_code": code}
        assert code == 0, (name, code, result / (name + ".log"))
        values = subprocess.check_output([str(root / "out/bin/naki-cache-marker")], text=True,
                                          env=environment, timeout=5).split()
        manifest = json.loads((cache / "install/share/naki/ffmpeg-build.json").read_text())
        return {"marker": list(map(int, values)), "identity": manifest["identity"]}

    first = build("first")
    assert first["marker"] == [1, 11], first
    stamp = source.stat()
    source.write_text("#include <vvenc/marker.h>\nint format_marker(void) { return 2+VVENC_WRAPPER_MARKER; }\n")
    os.utime(source, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    assert source.stat().st_mtime_ns == stamp.st_mtime_ns
    changed_ffmpeg = build("same-mtime-ffmpeg")
    ffmpeg_skip = build("same-mtime-ffmpeg-skip", skip=True)
    stamp = vvenc_source.stat()
    vvenc_source.write_text("int vvenc_marker(void) { return 12; }\n")
    os.utime(vvenc_source, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    assert vvenc_source.stat().st_mtime_ns == stamp.st_mtime_ns
    header_stamp = vvenc_header.stat()
    vvenc_header.write_text("int vvenc_marker(void); /* 12 */\n#define VVENC_WRAPPER_MARKER 0\n")
    os.utime(vvenc_header, ns=(header_stamp.st_atime_ns, header_stamp.st_mtime_ns))
    changed_vvenc = build("same-mtime-vvenc")
    final_skip = build("same-mtime-vvenc-skip", skip=True)
    installed_header = (cache / "install/include/vvenc/marker.h").read_text()
    report = {"first": first, "ffmpeg_changed": changed_ffmpeg, "ffmpeg_skip": ffmpeg_skip,
              "vvenc_changed": changed_vvenc, "final_skip": final_skip, "vvenc_header": installed_header}
    (result / "observations.json").write_text(json.dumps(report, indent=2) + "\n")
    assert first["identity"] != changed_ffmpeg["identity"] != changed_vvenc["identity"]
    if expect_stale:
        assert changed_ffmpeg["marker"] == ffmpeg_skip["marker"] == [1, 11], report
        assert changed_vvenc["marker"] == final_skip["marker"] == [1, 11], report
        print("REPRODUCED stale DSOs sealed as changed sources and accepted by skip:", result, flush=True)
        return
    else:
        assert changed_ffmpeg["marker"] == ffmpeg_skip["marker"] == [2, 11], report
        assert changed_vvenc["marker"] == final_skip["marker"] == [2, 12], report
        assert installed_header == vvenc_header.read_text(), report
        print("PASS same-mtime source changes rebuild actual FFmpeg/VVenC DSOs, seal and skip:", result, flush=True)

    # A real downstream source includes the installed upstream public header.
    # Only its content changes; source and installed timestamps intentionally collide.
    old_header=vvenc_header.read_bytes();header_stamp=vvenc_header.stat()
    vvenc_header.write_bytes(old_header.replace(b"WRAPPER_MARKER 0",b"WRAPPER_MARKER 100"))
    os.utime(vvenc_header,ns=(header_stamp.st_atime_ns,header_stamp.st_mtime_ns))
    installed=cache/"install/include/vvenc/marker.h"
    os.utime(installed,ns=(header_stamp.st_atime_ns,header_stamp.st_mtime_ns))
    before_key=json.loads((cache/"install/share/naki/ffmpeg-build.json").read_text())["object_keys"]["ffmpeg"]
    header_changed=build("transitive-public-header-same-mtime")
    after_key=json.loads((cache/"install/share/naki/ffmpeg-build.json").read_text())["object_keys"]["ffmpeg"]
    report["transitive_public_header"]={"before_key":before_key,"after_key":after_key,**header_changed}
    if expect_stale_transitive:
        assert header_changed["marker"]==[2,12] and before_key==after_key,report
        (result/"TRANSITIVE-BEFORE.json").write_text(json.dumps(report,indent=2)+"\n")
        print("REPRODUCED unchanged downstream object key and stale wrapper after old-mtime upstream header change:",result,flush=True)
        return
    assert header_changed["marker"]==[102,12] and before_key!=after_key,report
    vvenc_header.write_bytes(old_header);os.utime(vvenc_header,ns=(header_stamp.st_atime_ns,header_stamp.st_mtime_ns))
    assert build("restore-public-header")["marker"]==[2,12]
    print("PASS actual downstream wrapper behavior follows upstream header content with preserved mtime:",result,flush=True)

    baseline = {path: (path.read_bytes(), path.stat().st_mtime_ns)
                for path in (source, ffmpeg_broken, vvenc_source, vvenc_broken)}

    def restore():
        for path, (content, stamp) in baseline.items():
            path.write_bytes(content)
            os.utime(path, ns=(stamp, stamp))
            assert path.stat().st_mtime_ns == stamp

    def no_trusted_receipts(name):
        assert not (cache / ".naki-object-keys.json").exists(), name
        assert not any((cache / component / ".naki-object-key").exists()
                       for component in ("ffmpeg", "vvenc")), name
        assert not (cache / "install/share/naki/ffmpeg-build.json").exists(), name

    def partial_marker(name, component, value):
        obj = (cache / "ffmpeg/demux.o" if component == "ffmpeg" else
               cache / "vvenc/CMakeFiles/vvenc.dir/marker.c.o")
        library = result / (name + "-partial.so")
        subprocess.run(["gcc", "-shared", str(obj), "-o", str(library)], check=True, timeout=10)
        # Separate process avoids dlopen retaining an earlier version of a DSO.
        symbol = "format_marker" if component == "ffmpeg" else "vvenc_marker"
        observed = subprocess.check_output(["/usr/bin/python3", "-c",
            "import ctypes,sys; print(getattr(ctypes.CDLL(sys.argv[1]),sys.argv[2])())",
            str(library), symbol], text=True, timeout=5)
        assert int(observed) == value, (name, observed)
        return int(observed)

    recovery = []
    for component, marker, broken, symbol, value in (
            ("ffmpeg", source, ffmpeg_broken, "format_marker", 22),
            ("vvenc", vvenc_source, vvenc_broken, "vvenc_marker", 32)):
        for mode in ("compile-failure", "sigterm", "sigkill"):
            name = component + "-" + mode
            marker.write_text(f"int {symbol}(void) {{ return {value}; }}\n")
            extra = {}
            interrupt = None
            if mode == "compile-failure":
                broken.write_text("this is deliberately invalid C;\n")
            else:
                extra = {"SDK_TEST_TARGET": str(broken), "SDK_TEST_BARRIER": str(result / (name + ".ready"))}
                interrupt = signal.SIGTERM if mode == "sigterm" else signal.SIGKILL
            failed = build(name, extra=extra, fail=True, interrupt=interrupt)
            partial = partial_marker(name, component, value)
            no_trusted_receipts(name)
            restore()
            # Installed A may still be intact; the failed transaction is not a
            # sealed SDK. It must not authorize skip before the recovery build.
            build(name + "-skip-rejected", skip=True, fail=True)
            recovered = build(name + "-recover-A")
            skipped = build(name + "-recovered-skip", skip=True)
            assert recovered["marker"] == skipped["marker"] == [2, 12], (name, recovered, skipped)
            recovery.append({"case": name, **failed, "partial_marker": partial,
                             "recovered": recovered, "skip": skipped})

    # Let both real libraries build/install, then change a source during install
    # so the *actual seal source check* fails, rather than mocking a failed seal.
    source.write_text("int format_marker(void) { return 22; }\n")
    failed = build("seal-source-check-failure", extra={"SDK_TEST_MUTATE_SOURCE": str(vvenc_source)}, fail=True)
    assert "Dependency inputs changed during compilation" in (result / "seal-source-check-failure.log").read_text()
    no_trusted_receipts("seal-source-check-failure")
    restore()
    build("seal-failure-skip-rejected", skip=True, fail=True)
    recovered = build("seal-failure-recover-A")
    skipped = build("seal-failure-recovered-skip", skip=True)
    assert recovered["marker"] == skipped["marker"] == [2, 12], (recovered, skipped)
    recovery.append({"case": "seal-source-check-failure", **failed, "recovered": recovered, "skip": skipped})
    configuration_recovery = []
    configure_a = configure.read_bytes()
    configure_stamp = configure.stat().st_mtime_ns
    key_file = cache / "ffmpeg/.naki-configure-key"
    key_a = key_file.read_text()
    calls_file = cache / "ffmpeg/configure-calls.jsonl"
    for mode in ("failure", "sigterm", "sigkill"):
        name = "configure-" + mode
        configure.write_bytes(configure_a.replace(b"configuration_marker = 0", b"configuration_marker = 100"))
        assert configure.read_bytes() != configure_a
        extra = {"SDK_TEST_CONFIGURE_MODE": "fail" if mode == "failure" else "pause",
                 "SDK_TEST_BARRIER": str(result / (name + ".ready"))}
        interrupt = {"failure": None, "sigterm": signal.SIGTERM, "sigkill": signal.SIGKILL}[mode]
        failed = build(name, extra=extra, fail=True, interrupt=interrupt)
        # Observe actual generated B state, not just an injected exit code.
        header_b = (cache / "ffmpeg/config.h").read_text()
        make_b = (cache / "ffmpeg/ffbuild/config.mak").read_text()
        assert "FIXTURE_HEADER_MARKER 100" in header_b, (name, header_b)
        assert "FIXTURE_MAKE_MARKER := 100" in make_b, (name, make_b)
        assert not key_file.exists(), (name, "old configure receipt survived")
        assert not key_file.with_name(".naki-configure-key.pending").exists(), name
        no_trusted_receipts(name)
        configure.write_bytes(configure_a)
        os.utime(configure, ns=(configure_stamp, configure_stamp))
        assert configure.stat().st_mtime_ns == configure_stamp
        build(name + "-skip-rejected", skip=True, fail=True)
        calls_before = calls_file.read_text().splitlines()
        assert calls_before[-1] == "100", (name, calls_before)
        recovered = build(name + "-recover-A")
        calls_after = calls_file.read_text().splitlines()
        assert calls_after == calls_before + ["0"], (name, calls_before, calls_after)
        assert key_file.read_text() == key_a, name
        assert "FIXTURE_HEADER_MARKER 0" in (cache / "ffmpeg/config.h").read_text()
        assert "FIXTURE_MAKE_MARKER := 0" in (cache / "ffmpeg/ffbuild/config.mak").read_text()
        skipped = build(name + "-recovered-skip", skip=True)
        assert recovered["marker"] == skipped["marker"] == [2, 12], (name, recovered, skipped)
        configuration_recovery.append({"case": name, **failed, "generated_header_b": header_b,
                                       "generated_make_b": make_b, "recovered": recovered, "skip": skipped})
    report["failure_recovery"] = recovery
    report["configuration_recovery"] = configuration_recovery
    (result / "COMPLETE.json").write_text(json.dumps({"status": "passed", **report}, indent=2) + "\n")
    print("PASS real partial compile failures, SIGTERM/SIGKILL and seal failure recover A with old mtimes:", result, flush=True)
    print("PASS rewritten FFmpeg configuration failure/SIGTERM/SIGKILL reconfigure restored A:", result, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expect-stale", action="store_true", help="reproduce the pre-fix bug")
    parser.add_argument("--identity-script",type=pathlib.Path)
    parser.add_argument("--expect-stale-transitive",action="store_true")
    args=parser.parse_args()
    run(args.expect_stale,args.identity_script,args.expect_stale_transitive)
