#!/usr/bin/env python3
"""SDK reuse must reject stale sources, changed recipes and corrupted artifacts."""
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("sdk_identity", ROOT / "scripts/sdk-identity.py")
sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdk)


class ReuseTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="sdk-identity-", dir=ROOT / "out")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        for directory in sdk.DEPENDENCIES.values():
            source = self.root / directory
            source.mkdir(parents=True)
            (source / "source.c").write_text("original source\n")
            # No dependency Git metadata is needed for local byte identity.
            (source / ".git").write_text("gitdir: /unavailable/private/metadata\n")
        for name in sdk.RECIPES:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("build recipe\n")
        self.prefix = self.root / "install"
        (self.prefix / "include").mkdir(parents=True)
        (self.prefix / "lib").mkdir()
        (self.prefix / "include/test.h").write_text("header\n")
        (self.prefix / "lib/libavformat.so.63").write_bytes(b"installed library")
        (self.prefix / "lib/libavformat.so").symlink_to("libavformat.so.63")
        value = sdk.sources(self.root)
        value["artifacts"] = sdk.artifacts(self.prefix)
        value["object_keys"] = {name: sdk.object_key(value, name) for name in sdk.DEPENDENCIES}
        sdk.atomic_json(self.prefix / sdk.MANIFEST, value)

    def test_unchanged_sdk(self):
        self.assertEqual(sdk.verify(self.root, self.prefix)["schema_version"], 2)

    def test_ffmpeg_object_key_includes_upstream_component(self):
        before=sdk.sources(self.root)
        upstream=self.root/sdk.DEPENDENCIES["vvenc"]/"source.c"
        stamp=upstream.stat();upstream.write_text("changed public header or ABI\n")
        os.utime(upstream,ns=(stamp.st_atime_ns,stamp.st_mtime_ns))
        after=sdk.sources(self.root)
        self.assertEqual(before["sources"]["ffmpeg"],after["sources"]["ffmpeg"])
        self.assertNotEqual(sdk.object_key(before,"ffmpeg"),sdk.object_key(after,"ffmpeg"))

    def test_same_timestamp_source_change_and_untracked_source(self):
        source = self.root / sdk.DEPENDENCIES["ffmpeg"] / "source.c"
        stamp = source.stat()
        source.write_text("modified source\n")
        os.utime(source, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        with self.assertRaisesRegex(ValueError, "sources/recipe changed"):
            sdk.verify(self.root, self.prefix)
        source.write_text("original source\n")
        (source.parent / "new.c").write_text("uncommitted source")
        with self.assertRaisesRegex(ValueError, "sources/recipe changed"):
            sdk.verify(self.root, self.prefix)

    def test_recipe_change(self):
        (self.root / sdk.RECIPES[0]).write_text("different compiler recipe")
        with self.assertRaisesRegex(ValueError, "sources/recipe changed"):
            sdk.verify(self.root, self.prefix)

    def test_changed_or_missing_artifact(self):
        library = self.prefix / "lib/libavformat.so.63"
        library.write_bytes(b"stale or corrupt installed DSO")
        with self.assertRaisesRegex(ValueError, "bytes differ"):
            sdk.verify(self.root, self.prefix)
        library.write_bytes(b"installed library")
        (self.prefix / "include/test.h").unlink()
        with self.assertRaisesRegex(ValueError, "bytes differ"):
            sdk.verify(self.root, self.prefix)

    def test_exported_sdk_without_sources(self):
        # Container application stage has the sealed SDK but neither source tree.
        root = self.root / "application-stage"
        root.mkdir()
        sdk.verify(root, self.prefix)
        (root / sdk.DEPENDENCIES["vvenc"]).mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, "part of the dependency"):
            sdk.verify(root, self.prefix)

    def test_missing_or_incomplete_manifest(self):
        manifest = self.prefix / sdk.MANIFEST
        manifest.unlink()
        with self.assertRaisesRegex(ValueError, "provenance is missing"):
            sdk.verify(self.root, self.prefix)
        manifest.write_text(json.dumps({"schema_version": 2}))
        with self.assertRaisesRegex(ValueError, "bytes differ"):
            sdk.verify(self.root, self.prefix)

    def test_missing_object_receipt(self):
        manifest = self.prefix / sdk.MANIFEST
        value = json.loads(manifest.read_text())
        del value["object_keys"]
        manifest.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, "object receipts"):
            sdk.verify(self.root, self.prefix)

    def test_pending_component_keys_do_not_authorize_reuse(self):
        value = json.loads((self.prefix / sdk.MANIFEST).read_text())
        for name, key in value["object_keys"].items():
            directory = self.root / name
            directory.mkdir()
            (directory / ".naki-object-key.pending").write_text(key)
            (directory / ".naki-object-key").write_text(key)  # legacy receipt
            self.assertEqual(sdk.cache_key(self.root, self.prefix, name), "")

    def test_committed_cache_key_requires_matching_manifest(self):
        value = json.loads((self.prefix / sdk.MANIFEST).read_text())
        sdk.atomic_json(self.root / sdk.CACHE_RECEIPT,
                        {"identity": value["identity"], "object_keys": value["object_keys"]})
        self.assertEqual(sdk.cache_key(self.root, self.prefix, "ffmpeg"), value["object_keys"]["ffmpeg"])
        (self.prefix / sdk.MANIFEST).unlink()
        self.assertEqual(sdk.cache_key(self.root, self.prefix, "ffmpeg"), "")

    def test_invalidation_retires_committed_legacy_and_pending_keys(self):
        (self.root / sdk.CACHE_RECEIPT).write_text("committed cache")
        for name in sdk.DEPENDENCIES:
            directory = self.root / name
            directory.mkdir()
            for suffix in ("", ".pending"):
                (directory / (".naki-object-key" + suffix)).write_text("old key")
        sdk.invalidate_cache(self.root, self.prefix)
        self.assertFalse((self.root / sdk.CACHE_RECEIPT).exists())
        self.assertFalse((self.prefix / sdk.MANIFEST).exists())
        for name in sdk.DEPENDENCIES:
            self.assertEqual(list((self.root / name).iterdir()), [])

    def test_direct_seal_failure_retires_previous_commit(self):
        value = sdk.sources(self.root)
        snapshot = self.root / "snapshot.json"
        sdk.atomic_json(snapshot, value)
        installed = json.loads((self.prefix / sdk.MANIFEST).read_text())
        sdk.atomic_json(self.root / sdk.CACHE_RECEIPT,
                        {"identity": installed["identity"], "object_keys": installed["object_keys"]})
        (self.root / sdk.DEPENDENCIES["ffmpeg"] / "source.c").write_text("changed during build\n")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/sdk-identity.py"), "seal",
                                 "--root", str(self.root), "--prefix", str(self.prefix),
                                 "--snapshot", str(snapshot), "--build-root", str(self.root)],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Dependency inputs changed during compilation", result.stderr)
        self.assertFalse((self.root / sdk.CACHE_RECEIPT).exists())
        self.assertFalse((self.prefix / sdk.MANIFEST).exists())

    @unittest.skipUnless(shutil.which("git"), "release-key Git fixture runs on the CI host")
    def test_release_key_requires_committed_matching_inputs(self):
        repository = self.root / "release"
        repository.mkdir()

        def git(directory, *args):
            return subprocess.check_output(["git", "-C", str(directory), "-c", "user.name=SDK Test",
                                            "-c", "user.email=sdk-test@invalid", *args], text=True).strip()

        git(repository, "init", "--quiet")
        for name in (*sdk.RECIPES, "scripts/ffmpeg-key.sh"):
            path = repository / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("committed recipe\n")
        git(repository, "add", *sdk.RECIPES, "scripts/ffmpeg-key.sh")
        for relative in sdk.DEPENDENCIES.values():
            source = repository / relative
            source.mkdir(parents=True)
            git(source, "init", "--quiet")
            (source / "source.c").write_text("committed source\n")
            git(source, "add", "source.c")
            git(source, "commit", "--quiet", "-m", "fixture")
            git(repository, "update-index", "--add", "--cacheinfo",
                "160000", git(source, "rev-parse", "HEAD"), relative)
        git(repository, "commit", "--quiet", "-m", "fixture dependency pins")
        first_key = sdk.release_key(repository)
        self.assertEqual(len(first_key), 24)
        source = repository / sdk.DEPENDENCIES["ffmpeg"]
        (source / "source.c").write_text("changed dependency\n")
        with self.assertRaisesRegex(ValueError, "uncommitted sources"):
            sdk.release_key(repository)
        git(source, "add", "source.c")
        git(source, "commit", "--quiet", "-m", "next library")
        with self.assertRaisesRegex(ValueError, "does not match its parent gitlink"):
            sdk.release_key(repository)
        git(repository, "add", sdk.DEPENDENCIES["ffmpeg"])
        git(repository, "commit", "--quiet", "-m", "pin next library")
        self.assertNotEqual(first_key, sdk.release_key(repository))
        (repository / sdk.RECIPES[0]).write_text("uncommitted recipe\n")
        with self.assertRaisesRegex(ValueError, "recipes are uncommitted"):
            sdk.release_key(repository)


if __name__ == "__main__":
    unittest.main()
