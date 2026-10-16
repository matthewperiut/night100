#!/usr/bin/env python3
"""The restricted FFmpeg recipe must retain elementary MPEG probing for PSS."""
import contextlib
import importlib.util
import io
import json
import subprocess
import difflib
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SPEC=importlib.util.spec_from_file_location('switch_video_bootstrap',Path(__file__).resolve().parents[1]/'tools/bootstrap_switch_video.py')
TOOL=importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


class BootstrapChecks(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root=Path(self.directory.name)
        self.dependency={'path':'vendor/ffmpeg-switch','commit':'pinned','branch_hint':'nvtegra','url':'unused'}
        (self.root/self.dependency['path']).mkdir(parents=True)
        (self.root/'dependencies-switch-video.json').write_text(json.dumps({'ffmpeg-nvtegra':self.dependency}))
        (self.root/'build/switch-ffmpeg/install/lib').mkdir(parents=True)
        self.commands=[]
        self.receipts=[]

    def invoke(self):
        def output(command,**kwargs):
            if command[-1]=='--version': return 'fixture compiler\n'
            if command[-2:]==['rev-parse','HEAD']: return 'pinned\n'
            if command[-2:]==['status','--porcelain']: return ''
            self.fail('Unexpected command '+repr(command))
        with mock.patch.object(TOOL,'ROOT',self.root),mock.patch.object(TOOL,'apply_compatibility_patches',return_value=self.receipts),mock.patch.object(TOOL,'run',side_effect=lambda command,**kwargs:self.commands.append([str(x)for x in command])),mock.patch.object(TOOL.subprocess,'check_output',side_effect=output),mock.patch('sys.argv',['bootstrap_switch_video.py','--devkitpro',str(self.root/'devkit'),'--jobs','1']),contextlib.redirect_stdout(io.StringIO()):
            TOOL.main()

    def configure_calls(self):
        return [c for c in self.commands if c[0].endswith('/configure')]

    def test_mpegps_elementary_probe_and_hardware_only_recipe(self):
        self.invoke()
        configure=self.configure_calls()
        self.assertEqual(len(configure),1)
        demuxers=set()
        for flag in configure[0]:
            if flag.startswith('--enable-demuxer='):
                demuxers.update(flag.split('=',1)[1].split(','))
        # MPEG-PS stream probing delegates to the raw elementary MPEG demuxer;
        # leaving it out reproduces valid PSS movies as codec NONE, size0x0.
        self.assertTrue({'mpegps','mpegvideo'}<=demuxers)
        self.assertIn('--enable-parser=mpegvideo',configure[0])
        self.assertIn('--enable-decoder=mpeg2video',configure[0])
        self.assertIn('--enable-hwaccel=mpeg2_nvtegra',configure[0])
        self.assertIn('--disable-everything',configure[0])
        self.assertIn('--disable-programs',configure[0])

    def test_old_broken_configuration_is_rebuilt_then_cached(self):
        self.invoke()
        marker=self.root/'build/switch-ffmpeg/night100-config.json'
        broken=json.loads(marker.read_text())
        broken['flags']=[f.replace('--enable-demuxer=mpegps,mpegvideo','--enable-demuxer=mpegps')for f in broken['flags']]
        marker.write_text(json.dumps(broken))
        self.commands.clear()
        self.invoke()
        self.assertEqual(len(self.configure_calls()),1)
        self.commands.clear()
        self.invoke()
        self.assertEqual(self.configure_calls(),[])
        self.assertEqual(self.commands,[['make','-j1'],['make','install']])

    def test_patch_content_hash_invalidates_existing_build(self):
        self.receipts=[{'path':'patches/fix.patch','sha256':'before'}]
        self.invoke()
        self.receipts=[{'path':'patches/fix.patch','sha256':'after'}]
        self.commands.clear()
        self.invoke()
        self.assertEqual(len(self.configure_calls()),1)
        installed=json.loads((self.root/'build/switch-ffmpeg/install/night100-build.json').read_text())
        self.assertEqual(installed['patches'],self.receipts)


class CompatibilityPatchChecks(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root=Path(self.directory.name)
        self.source=self.root/'vendor/ffmpeg'
        self.source.mkdir(parents=True)
        self.original='module=(MmuModuleId)5;\n'
        self.changed='module=MmuModuleId_Nvdec;\n'
        (self.source/'clock.c').write_text(self.original)
        (self.source/'other.c').write_text('preserve me\n')
        self.git('init','-q')
        self.git('add','clock.c','other.c')
        self.git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','fixture')
        self.revision=self.git('rev-parse','HEAD').decode().strip()
        (self.root/'patches').mkdir()
        self.patch=self.root/'patches/clock.patch'
        self.patch.write_text(''.join(difflib.unified_diff(self.original.splitlines(True),self.changed.splitlines(True),fromfile='a/clock.c',tofile='b/clock.c')))
        self.dependency={'commit':self.revision,'patches':['patches/clock.patch']}

    def git(self,*args):
        return subprocess.check_output(['git','-C',str(self.source),*args])

    def apply(self):
        with mock.patch.object(TOOL,'ROOT',self.root):
            return TOOL.apply_compatibility_patches(self.source,self.dependency)

    def test_pristine_apply_idempotent_reconstruction_and_receipt(self):
        first=self.apply()
        self.assertEqual((self.source/'clock.c').read_text(),self.changed)
        self.assertEqual(self.apply(),first)
        self.assertEqual(first[0]['path'],'patches/clock.patch')
        self.assertEqual(len(first[0]['sha256']),64)
        self.assertEqual(self.git('diff','--cached'),b'')
        self.git('add','clock.c')
        self.assertEqual(self.apply(),first)

    def test_unrelated_tracked_edits_are_preserved_and_rejected(self):
        (self.source/'other.c').write_text('user work\n')
        with self.assertRaisesRegex(SystemExit,'differs'):
            self.apply()
        self.assertEqual((self.source/'other.c').read_text(),'user work\n')
        self.assertEqual((self.source/'clock.c').read_text(),self.original)

    def test_modified_patch_target_is_preserved_and_rejected(self):
        (self.source/'clock.c').write_text('module=user_selected;\n')
        with self.assertRaisesRegex(SystemExit,'differs'):
            self.apply()
        self.assertEqual((self.source/'clock.c').read_text(),'module=user_selected;\n')

    def test_hidden_staged_changes_are_rejected(self):
        (self.source/'other.c').write_text('staged user work\n')
        self.git('add','other.c')
        (self.source/'other.c').write_text('preserve me\n')
        self.assertEqual(self.git('diff',self.revision),b'')
        with self.assertRaisesRegex(SystemExit,'differs'):
            self.apply()
        self.assertIn(b'staged user work',self.git('show',':other.c'))

    def test_untracked_file_is_preserved_and_rejected(self):
        (self.source/'local.c').write_text('untracked user work\n')
        with self.assertRaisesRegex(SystemExit,'untracked'):
            self.apply()
        self.assertEqual((self.source/'local.c').read_text(),'untracked user work\n')

    def test_listed_partial_patch_set_finishes_in_order(self):
        second='another=1;\n'
        (self.root/'patches/second.patch').write_text(''.join(difflib.unified_diff(['preserve me\n'],[second],fromfile='a/other.c',tofile='b/other.c')))
        self.apply()
        self.dependency['patches'].append('patches/second.patch')
        receipts=self.apply()
        self.assertEqual(len(receipts),2)
        self.assertEqual((self.source/'clock.c').read_text(),self.changed)
        self.assertEqual((self.source/'other.c').read_text(),second)
        self.assertEqual(self.apply(),receipts)

    def test_unlisted_patch_hash_change_is_detected(self):
        first=self.apply()
        alternative='module=MmuModuleId_Nvdec; // audited\n'
        self.patch.write_text(''.join(difflib.unified_diff(self.original.splitlines(True),alternative.splitlines(True),fromfile='a/clock.c',tofile='b/clock.c')))
        with self.assertRaisesRegex(SystemExit,'differs'):
            self.apply()
        (self.source/'clock.c').write_text(self.original)
        changed=self.apply()
        self.assertNotEqual(first[0]['sha256'],changed[0]['sha256'])
        self.assertEqual((self.source/'clock.c').read_text(),alternative)


if __name__=='__main__': unittest.main()
