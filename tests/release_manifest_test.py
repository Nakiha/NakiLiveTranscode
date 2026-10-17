#!/usr/bin/env python3
"""Release binding: actual ELF/DWARF checks plus source/image/byte rejection."""
import argparse
import copy
import importlib.util
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch
ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('release_manifest',ROOT/'scripts/release-manifest.py');release=importlib.util.module_from_spec(spec);spec.loader.exec_module(release)
class Binding(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary=tempfile.TemporaryDirectory(prefix='release-binding-',dir=ROOT/'out');cls.directory=pathlib.Path(cls.temporary.name)
        cls.binary=cls.directory/'module';cls.other=cls.directory/'other/module';cls.other.parent.mkdir()
        source=cls.directory/'fixture.c';source.write_text('int main(void) { return VALUE; }\n')
        for value,output in enumerate((cls.binary,cls.other)):
            subprocess.run(['cc','-g','-O0','-Wl,--build-id=sha1',f'-DVALUE={value}',str(source),'-o',str(output)],check=True,capture_output=True,timeout=30)
        cls.symbols=cls.directory/'symbols';release.symbols.archive(cls.symbols,[cls.binary],'fixture','a'*40,'key')
        cls.source={'git_revision':'a'*40,'dirty':False,'inputs':{'files':{'app/main.c':'b'*64},'gitlinks':{}}}
        cls.source['tree_sha256']=release.canonical(cls.source['inputs'])
    @classmethod
    def tearDownClass(cls):cls.temporary.cleanup()
    def image(self):
        return [{'Os':'linux','Architecture':'amd64','Id':'sha256:'+'c'*64,'RepoTags':['naki-live-transcode:'+'a'*40+'-123-2'],
            'Config':{'Labels':{'org.opencontainers.image.revision':'a'*40,'io.naki.source-tree':self.source['tree_sha256'],'io.naki.build-run':'123','io.naki.build-attempt':'2'}}}]
    def inspect(self,value):
        path=self.directory/'image.json';path.write_text(json.dumps(value));return release.image_record(path,self.source,'123','2')
    def test_actual_elf_matches_debug(self):
        index=release.index_symbols(self.symbols);row=release.bind_module(self.binary,index,'fixture')
        self.assertEqual(row['runtime_elf_sha256'],release.digest(self.binary));self.assertTrue(row['debug_sha256'])
    def test_changed_runtime_build_id_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'matching debug'):release.bind_module(self.other,release.index_symbols(self.symbols),'fixture')
    def test_corrupt_dwarf_is_rejected(self):
        manifest=json.loads((self.symbols/'manifest.json').read_text());path=self.symbols/manifest['modules'][0]['file'];original=path.read_bytes()
        try:
            path.write_bytes(original+b'corrupt')
            with self.assertRaisesRegex(ValueError,'Debug bytes'):release.index_symbols(self.symbols)
        finally:path.write_bytes(original)
    def test_source_digest_rejects_changed_input(self):
        value=copy.deepcopy(self.source);release.check_source(value);value['inputs']['files']['app/main.c']='d'*64
        with self.assertRaisesRegex(ValueError,'digest'):release.check_source(value)
    def test_yyjson_gitlink_worktree_bytes_are_included_even_with_old_mtime(self):
        root=self.directory/'source-capture';(root/'app').mkdir(parents=True);(root/'app/main.c').write_text('int app;\n')
        source=root/'third_party/yyjson/src/yyjson.c';source.parent.mkdir(parents=True);source.write_text('int json=1;\n')
        def fake_git(directory,*args):
            if args[0]=='ls-files':return b'app/main.c\0'
            if args[0]=='status':return b''
            return ('a'*40 if args[1]=='HEAD' and directory==root else 'b'*40).encode()
        with patch.object(release,'git',side_effect=fake_git):
            first=release.capture_source(root);timestamp=source.stat();source.write_text('int json=2;\n');os.utime(source,ns=(timestamp.st_atime_ns,timestamp.st_mtime_ns))
            second=release.capture_source(root)
        self.assertNotEqual(first['tree_sha256'],second['tree_sha256']);self.assertEqual(first['inputs']['gitlinks'],second['inputs']['gitlinks'])
        self.assertNotEqual(first['inputs']['files']['third_party/yyjson/src/yyjson.c'],second['inputs']['files']['third_party/yyjson/src/yyjson.c'])
    def test_unique_tag_and_actual_image_id(self):
        result=self.inspect(self.image());self.assertEqual(result['image_id'],'sha256:'+'c'*64)
        image=self.image();image[0]['RepoTags']=['naki-live-transcode:'+'a'*40]
        with self.assertRaisesRegex(ValueError,'Unique'):self.inspect(image)
    def test_wrong_attempt_or_tree_or_revision_is_rejected(self):
        for label in ('org.opencontainers.image.revision','io.naki.source-tree','io.naki.build-attempt'):
            image=self.image();image[0]['Config']['Labels'][label]='wrong'
            with self.subTest(label=label),self.assertRaisesRegex(ValueError,'labels'):self.inspect(image)
    def test_wrong_platform_and_ambiguous_image_rejected(self):
        image=self.image();image[0]['Architecture']='arm64'
        with self.assertRaisesRegex(ValueError,'target'):self.inspect(image)
        with self.assertRaisesRegex(ValueError,'one image'):self.inspect(self.image()*2)
    def test_image_sdk_label_must_match_selected_dependency(self):
        image=self.image();image[0]['Config']['Labels']['io.naki.ffmpeg-dependency']='sdk-A'
        path=self.directory/'sdk-image.json';path.write_text(json.dumps(image))
        release.image_record(path,self.source,'123','2','sdk-A')
        with self.assertRaisesRegex(ValueError,'labels'):release.image_record(path,self.source,'123','2','sdk-B')
    def test_manifest_artifact_bytes_cannot_change(self):
        artifact=self.directory/'image.tar.gz';artifact.write_bytes(b'candidate image bytes')
        manifest={'source':self.source,'modules':[],'sdk':{'identity':'test'},'image':None,'publication_eligible':False,'artifacts':[release.artifact(artifact)]}
        prefix=self.directory/'install';sdk_manifest=prefix/release.sdk.sdk.MANIFEST;sdk_manifest.parent.mkdir(parents=True,exist_ok=True);sdk_manifest.write_text('{"identity":"test"}')
        manifest_path=self.directory/'candidate.json';manifest_path.write_text(json.dumps(manifest))
        args=argparse.Namespace(manifest=manifest_path,runtime_bin=None,sdk_prefix=prefix,app_symbols=None,ffmpeg_symbols=None,vvenc_symbols=None,image_inspect=None,artifact_directory=self.directory)
        with patch.object(release,'bind_runtime',return_value=[]):
            release.verify(args);artifact.write_bytes(b'different bytes')
            with self.assertRaisesRegex(ValueError,'artifact bytes'):release.verify(args)
    def test_dirty_or_no_image_cannot_claim_publication(self):
        manifest={'source':dict(self.source,dirty=True),'modules':[],'sdk':{'identity':'test'},'image':None,'publication_eligible':True,'artifacts':[]}
        prefix=self.directory/'eligibility-install';path=prefix/release.sdk.sdk.MANIFEST;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('{"identity":"test"}')
        file=self.directory/'eligibility.json';file.write_text(json.dumps(manifest));args=argparse.Namespace(manifest=file,runtime_bin=None,sdk_prefix=prefix,app_symbols=None,ffmpeg_symbols=None,vvenc_symbols=None,image_inspect=None,artifact_directory=None)
        with patch.object(release,'bind_runtime',return_value=[]),self.assertRaisesRegex(ValueError,'eligibility'):release.verify(args)
if __name__=='__main__':unittest.main()
