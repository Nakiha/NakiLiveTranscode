#!/usr/bin/env python3
"""Bind a candidate's source, SDK, runtime ELF, separate DWARF and image bytes.

Creation is local/read-only except its explicit output. No release, image push,
tag creation or deployment happens here. A dirty/no-image candidate is local.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT=pathlib.Path(__file__).resolve().parents[1]
PRODUCTION=('naki-live','naki-live-worker','naki-live-ctl','naki-crash-collector')
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
symbols=load('naki_release_symbols',ROOT/'scripts/archive-symbols.py')
sdk=load('naki_release_sdk',ROOT/'scripts/sdk-expected.py')
def digest(path):
    with pathlib.Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def canonical(value):return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()
def git(root,*args):
    return subprocess.run(['git','-C',str(root),*args],check=True,capture_output=True,timeout=60,
        env=dict(os.environ,GIT_OPTIONAL_LOCKS='0')).stdout
def capture_source(root):
    revision=git(root,'rev-parse','HEAD').decode().strip()
    if not re.fullmatch('[0-9a-f]{40}',revision):raise ValueError('Invalid source revision')
    paths=['app','scripts','tests','.github','CMakeLists.txt','Dockerfile','Dockerfile.ffmpeg','compose.yaml']
    names=set(git(root,'ls-files','--cached','--others','--exclude-standard','-z','--',*paths).decode().split('\0'))-{''}
    files={name:digest(root/name) for name in sorted(names) if (root/name).is_file()}
    # Parent git ls-files does not descend into a gitlink. Bind the actual JSON
    # implementation bytes as well as the pin, including same-mtime changes.
    for path in sorted((root/'third_party/yyjson/src').rglob('*')):
        if path.is_symlink():raise ValueError('Unexpected yyjson source symlink')
        if path.is_file():files[path.relative_to(root).as_posix()]=digest(path)
    if not any(name.startswith('third_party/yyjson/src/') for name in files):raise ValueError('yyjson build source is missing')
    pins={name:git(root,'rev-parse','HEAD:'+name).decode().strip() for name in ('third_party/yyjson',*sdk.sdk.DEPENDENCIES.values())}
    tree={'files':files,'gitlinks':pins}
    # Avoid recursively statting native Windows/WSL trees here. Their clean
    # pinned histories and content are checked separately by the SDK gates.
    dirty=bool(git(root,'status','--porcelain','--untracked-files=normal','--ignore-submodules=all').strip())
    yyjson=root/'third_party/yyjson'
    dirty=dirty or git(yyjson,'rev-parse','HEAD').decode().strip()!=pins['third_party/yyjson'] or bool(git(yyjson,'status','--porcelain','--untracked-files=all').strip())
    return {'git_revision':revision,'dirty':dirty,'tree_sha256':canonical(tree),'inputs':tree}
def check_source(source):
    if source.get('tree_sha256')!=canonical(source.get('inputs')):raise ValueError('Source content digest is inconsistent')
    if not re.fullmatch('[0-9a-f]{40}',source.get('git_revision','')):raise ValueError('Source revision is missing')
def index_symbols(directory):
    directory=pathlib.Path(directory);manifest=json.loads((directory/'manifest.json').read_text());result={}
    for module in manifest['modules']:
        path=(directory/module['file']).resolve()
        if not path.is_relative_to(directory.resolve()):raise ValueError('Debug path escapes symbol archive')
        if digest(path)!=module['debug_sha256'] or symbols.build_id(path)!=module['build_id']:raise ValueError('Debug bytes/Build ID do not match their manifest')
        if '.debug_info' not in symbols.command('readelf','-SW',str(path)):raise ValueError('Debug file lacks DWARF')
        previous=result.get(module['build_id'])
        if previous and previous['debug_sha256']!=module['debug_sha256']:raise ValueError('Conflicting debug files for Build ID')
        result[module['build_id']]=module
    if not result:raise ValueError('Empty symbol archive')
    return result
def bind_module(path,index,component):
    path=pathlib.Path(path);identity=symbols.build_id(path);debug=index.get(identity)
    if debug is None or debug['name']!=path.name:raise ValueError('Missing matching debug module: '+path.name)
    return {'component':component,'name':path.name,'build_id':identity,'runtime_elf_sha256':digest(path),
        'debug_file':debug['file'],'debug_sha256':debug['debug_sha256']}
def bind_runtime(runtime_bin,prefix,symbol_directories):
    app,ffmpeg,vvenc=map(index_symbols,symbol_directories)
    rows=[bind_module(pathlib.Path(runtime_bin)/name,app,'application') for name in PRODUCTION]
    for component,index in (('ffmpeg',ffmpeg),('vvenc',vvenc)):
        modules=sorted(index.values(),key=lambda r:r['name'])
        for module in modules:
            # The native archives contain the shared modules used by this SDK.
            path=pathlib.Path(prefix)/'lib'/module['name']
            if not path.is_file():raise ValueError('SDK native module is absent: '+module['name'])
            rows.append(bind_module(path,index,component))
    return rows
def image_record(path,source,run_id,attempt,dependency_key=None):
    data=json.loads(pathlib.Path(path).read_text())
    if not isinstance(data,list) or len(data)!=1:raise ValueError('Image inspection must identify one image')
    item=data[0];labels=item.get('Config',{}).get('Labels',{}) or {}
    expected={'org.opencontainers.image.revision':source['git_revision'],'io.naki.source-tree':source['tree_sha256'],
        'io.naki.build-run':run_id,'io.naki.build-attempt':attempt}
    if dependency_key is not None:expected['io.naki.ffmpeg-dependency']=dependency_key
    if item.get('Os')!='linux' or item.get('Architecture')!='amd64':raise ValueError('Runtime image target is not Linux amd64')
    if any(labels.get(k)!=v for k,v in expected.items()):raise ValueError('Runtime image labels do not match source/run identity')
    if not re.fullmatch('sha256:[0-9a-f]{64}',item.get('Id','')):raise ValueError('Runtime image ID is missing')
    tag=f'naki-live-transcode:{source["git_revision"]}-{run_id}-{attempt}'
    if tag not in item.get('RepoTags',[]):raise ValueError('Unique source-run-attempt image tag is absent')
    return {'tag':tag,'image_id':item['Id'],'repo_digests':item.get('RepoDigests') or [],'os':'linux','architecture':'amd64','labels':expected}
def artifact(path):
    path=pathlib.Path(path);return {'name':path.name,'bytes':path.stat().st_size,'sha256':digest(path)}
def create(args):
    root=args.root.resolve();source=json.loads(args.source.read_text());check_source(source)
    if source!=capture_source(root):raise ValueError('Source inputs changed after the build identity was captured')
    for token in (args.run_id,args.attempt):
        if not re.fullmatch('[A-Za-z0-9_.-]{1,80}',token):raise ValueError('Invalid build run/attempt')
    if args.local:
        # Local engineering recipes may intentionally be dirty and content-
        # verified. Never mint a release-looking SDK key from those inputs.
        pins={name:git(root,'rev-parse','HEAD:'+path).decode().strip() for name,path in sdk.sdk.DEPENDENCIES.items()}
        for name,path in sdk.sdk.DEPENDENCIES.items():
            if git(root/path,'rev-parse','HEAD').decode().strip()!=pins[name] or git(root/path,'status','--porcelain','--untracked-files=all').strip():raise ValueError('Local native checkout must match its clean pinned history')
        inputs=sdk.sdk.sources(root)
        expectation={'source_identity':inputs['identity'],'dependency_key':'local-content-'+inputs['identity'][:24],'pins':pins}
    else:expectation=sdk.expected(root)
    if args.dependency_key and args.dependency_key!=expectation['dependency_key']:raise ValueError('Selected SDK key differs from trusted checkout')
    installed=sdk.verify(root,args.sdk_prefix.resolve())
    if installed['identity']!=expectation['source_identity']:raise ValueError('Installed SDK identity differs from expectation')
    modules=bind_runtime(args.runtime_bin,args.sdk_prefix,(args.app_symbols,args.ffmpeg_symbols,args.vvenc_symbols))
    image=image_record(args.image_inspect,source,args.run_id,args.attempt,expectation['dependency_key']) if args.image_inspect else None
    if image is None and not args.local:raise ValueError('A release candidate requires actual runtime image inspection')
    if image and (not args.image_archive or not args.symbols_archive):raise ValueError('Image/symbol bytes must accompany inspected runtime')
    artifacts=[artifact(path) for path in (args.image_archive,args.symbols_archive) if path]
    manifest={'schema_version':1,'target':sdk.TARGET,'source':source,'sdk':{'identity':installed['identity'],'dependency_key':expectation['dependency_key'],'pins':expectation['pins']},
        'build_run_id':args.run_id,'build_attempt':args.attempt,'image':image,'modules':modules,'artifacts':artifacts,
        'publication_eligible':bool(image and not source['dirty']),'scope':'local_review' if args.local else 'tested_image_candidate'}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x') as stream:json.dump(manifest,stream,indent=2);stream.write('\n')
    return manifest
def verify(args):
    manifest=json.loads(args.manifest.read_text());check_source(manifest['source'])
    rows=bind_runtime(args.runtime_bin,args.sdk_prefix,(args.app_symbols,args.ffmpeg_symbols,args.vvenc_symbols))
    if rows!=manifest['modules']:raise ValueError('Runtime ELF/debug modules differ from candidate manifest')
    installed=json.loads((args.sdk_prefix/sdk.sdk.MANIFEST).read_text())
    if installed['identity']!=manifest['sdk']['identity']:raise ValueError('SDK identity differs from candidate manifest')
    if args.image_inspect:
        if image_record(args.image_inspect,manifest['source'],manifest['build_run_id'],manifest['build_attempt'],manifest['sdk']['dependency_key'])!=manifest['image']:raise ValueError('Image inspection differs from candidate manifest')
    elif manifest['image'] is not None:raise ValueError('Image-bearing manifest requires image inspection')
    for expected in manifest['artifacts']:
        if not args.artifact_directory:raise ValueError('Artifact directory is required')
        name=expected['name']
        if pathlib.Path(name).name!=name:raise ValueError('Invalid artifact name')
        if artifact(args.artifact_directory/name)!=expected:raise ValueError('Candidate artifact bytes differ: '+name)
    if manifest['publication_eligible']!=bool(manifest['image'] and not manifest['source']['dirty']):raise ValueError('Publication eligibility is inconsistent')
    return manifest
def main():
    parser=argparse.ArgumentParser(description=__doc__);commands=parser.add_subparsers(dest='action',required=True)
    source=commands.add_parser('source');source.add_argument('--root',type=pathlib.Path,default=ROOT);source.add_argument('--output',type=pathlib.Path,required=True)
    for action in ('create','verify'):
        command=commands.add_parser(action)
        for name in ('runtime-bin','sdk-prefix','app-symbols','ffmpeg-symbols','vvenc-symbols'):command.add_argument('--'+name,type=pathlib.Path,required=True)
        command.add_argument('--image-inspect',type=pathlib.Path)
        if action=='create':
            command.add_argument('--root',type=pathlib.Path,default=ROOT);command.add_argument('--source',type=pathlib.Path,required=True)
            command.add_argument('--run-id',required=True);command.add_argument('--attempt',required=True);command.add_argument('--dependency-key')
            command.add_argument('--image-archive',type=pathlib.Path);command.add_argument('--symbols-archive',type=pathlib.Path)
            command.add_argument('--local',action='store_true');command.add_argument('--output',type=pathlib.Path,required=True)
        else:
            command.add_argument('--manifest',type=pathlib.Path,required=True);command.add_argument('--artifact-directory',type=pathlib.Path)
    args=parser.parse_args()
    if args.action=='source':
        value=capture_source(args.root.resolve());args.output.parent.mkdir(parents=True,exist_ok=True)
        with args.output.open('x') as stream:json.dump(value,stream,indent=2);stream.write('\n')
        print(value['tree_sha256'])
    else:
        value=create(args) if args.action=='create' else verify(args)
        print('PASS candidate manifest; publication_eligible='+str(value['publication_eligible']).lower())
if __name__=='__main__':
    try:main()
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError) as error:sys.exit('Release binding failed: '+str(error))
