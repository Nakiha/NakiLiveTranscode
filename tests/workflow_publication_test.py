#!/usr/bin/env python3
"""Run the real SDK workflow shell with local command stubs, never a release."""
import json
import os
import pathlib
import subprocess
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SDK = (ROOT / '.github/workflows/ffmpeg.yml').read_text()
SERVICE = (ROOT / '.github/workflows/service.yml').read_text()


def section(text, heading):
    """Select one indentation-delimited workflow block, without parsing YAML."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.strip() == heading:
            indent = len(line) - len(line.lstrip())
            end = index + 1
            while end < len(lines) and (not lines[end].strip() or
                    len(lines[end]) - len(lines[end].lstrip()) > indent):
                end += 1
            return '\n'.join(lines[index:end])
    raise AssertionError('Missing workflow block: ' + heading)


def command(name):
    lines = SDK.splitlines()
    start = next(i for i, line in enumerate(lines) if line.strip() == 'name: ' + name)
    run = next(i for i in range(start + 1, len(lines)) if lines[i].strip() == 'run: |')
    indent = len(lines[run]) - len(lines[run].lstrip())
    end = run + 1
    while end < len(lines) and (not lines[end].strip() or
            len(lines[end]) - len(lines[end].lstrip()) > indent):
        end += 1
    return textwrap.dedent('\n'.join(lines[run + 1:end])) + '\n'


STUB = '''#!/usr/bin/env python3
import json,os,pathlib,sys
name=pathlib.Path(sys.argv[0]).name
with open(os.environ['NAKI_TEST_COMMAND_LOG'],'a') as stream:
    stream.write(json.dumps({'command':name,'args':sys.argv[1:]})+'\\n')
if name=='bash':print('test-exact-sdk')
elif name=='git':print('a'*40)
elif name=='sha256sum':print('0'*64+'  out/ffmpeg-sdk.tar.gz')
elif name=='gh' and sys.argv[1:3]==['release','view']:
    if os.environ['NAKI_TEST_SDK_EXISTS']=='true':print('ffmpeg-sdk.tar.gz')
    else:sys.exit(1)
'''


class PublicationPolicy(unittest.TestCase):
    def test_boolean_defaults_and_service_cannot_enable_publication(self):
        for trigger in ('workflow_dispatch:', 'workflow_call:'):
            block = section(section(SDK, trigger), 'publish_sdk:')
            self.assertRegex(block, r'(?m)^\s+type: boolean$')
            self.assertRegex(block, r'(?m)^\s+default: false$')
        dependency = section(SERVICE, 'dependency:')
        self.assertRegex(dependency, r'(?m)^\s+uses: \./\.github/workflows/ffmpeg.yml$')
        self.assertRegex(section(dependency, 'with:'), r'(?m)^\s+publish_sdk: false$')
        self.assertIn('PUBLISH_SDK: ${{ inputs.publish_sdk }}', SDK)
        self.assertIn("- if: steps.key.outputs.missing == 'true'", SDK)
        self.assertLess(SDK.index('run: python3 tests/workflow_publication_test.py'),
                        SDK.index('name: Find matching dependency release'))

    def test_actual_lookup_and_publish_commands_for_all_entry_paths(self):
        lookup = command('Find matching dependency release')
        publish = command('Build and archive dependency once')
        events = ('push', 'pull_request', 'workflow_dispatch', 'workflow_call')
        values = ('false', 'true', '', 'False', 'TRUE', '0', '1', ' false ', ' true ')
        results = []
        with tempfile.TemporaryDirectory(prefix='workflow-publication-', dir=ROOT / 'out') as temporary:
            directory = pathlib.Path(temporary)
            stubs = directory / 'bin'
            stubs.mkdir()
            for name in ('bash', 'git', 'docker', 'tar', 'sha256sum', 'gh'):
                path = stubs / name
                path.write_text(STUB)
                path.chmod(0o755)
            for event in events:
                for value in values:
                    for exists in (False, True):
                        with self.subTest(event=event, publish_sdk=repr(value), exists=exists):
                            case = directory / str(len(list(directory.iterdir())))
                            (case / 'out').mkdir(parents=True)
                            log, output = case / 'commands.jsonl', case / 'outputs.txt'
                            env = dict(os.environ, PATH=str(stubs) + os.pathsep + os.environ['PATH'],
                                       NAKI_TEST_COMMAND_LOG=str(log), GITHUB_OUTPUT=str(output),
                                       NAKI_TEST_SDK_EXISTS='true' if exists else 'false',
                                       EVENT_NAME=event, PUBLISH_SDK=value, GH_TOKEN='test-only',
                                       TAG='ffmpeg-linux-amd64-test-exact-sdk', KEY='test-exact-sdk',
                                       GITHUB_SHA='b' * 40)
                            selected = subprocess.run(['/bin/bash', '--noprofile', '--norc', '-e', '-o',
                                                       'pipefail', '-c', lookup], cwd=case, env=env,
                                                      capture_output=True, text=True, timeout=10)
                            self.assertEqual(selected.returncode, 0, selected.stderr)
                            self.assertIn('missing=' + ('false' if exists else 'true'), output.read_text())
                            prior = log.read_text().splitlines()
                            if not exists:
                                result = subprocess.run(['/bin/bash', '--noprofile', '--norc', '-e', '-o',
                                                         'pipefail', '-c', publish], cwd=case, env=env,
                                                        capture_output=True, text=True, timeout=10)
                                allowed = value == 'true' and event != 'pull_request'
                                self.assertEqual(result.returncode, 0 if allowed else 1, result.stderr)
                            else:
                                allowed = False  # The existing workflow step condition skips publication.
                            added = [json.loads(line) for line in log.read_text().splitlines()[len(prior):]]
                            releases = [row for row in added if row['command'] == 'gh' and
                                        row['args'][:2] == ['release', 'create']]
                            self.assertEqual(len(releases), int(allowed))
                            if not allowed:
                                self.assertEqual(added, [], 'Forbidden path must stop before build or release')
                            else:
                                self.assertIn('ffmpeg-linux-amd64-test-exact-sdk', releases[0]['args'])
                                self.assertTrue(any(row['command'] == 'docker' for row in added))
                            results.append({'event': event, 'publish_sdk': value, 'sdk_exists': exists,
                                            'publication_step_skipped': exists, 'release_stub_calls': len(releases),
                                            'actual_sdk_publication': False, 'status': 'passed'})
        if os.environ.get('NAKI_WORKFLOW_POLICY_REPORT'):
            report = pathlib.Path(os.environ['NAKI_WORKFLOW_POLICY_REPORT']).resolve()
            self.assertTrue(report.is_relative_to(ROOT / 'out'))
            report.write_text(json.dumps({'cases': results, 'case_count': len(results),
                                          'commands': 'Actual workflow shell; build/network commands stubbed'},
                                         indent=2) + '\n')


if __name__ == '__main__':
    (ROOT / 'out').mkdir(exist_ok=True)
    unittest.main()
