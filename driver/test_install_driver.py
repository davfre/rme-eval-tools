import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch
spec=importlib.util.spec_from_file_location('installer',Path(__file__).with_name('install-driver.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class InstallerTests(unittest.TestCase):
    def test_parse_status(self):
        self.assertEqual(m.parse_status(f'{m.MODULE}/v1: added\n{m.MODULE}/v2, kernel, x86_64: installed\nunrelated/v: broken'), [('v1',None,'added'),('v2','kernel','installed')])
        with self.assertRaises(RuntimeError): m.parse_status(f'{m.MODULE}/v: broken')

    def test_existing_install_is_only_verified(self):
        checked=[]
        with patch.object(m,'dkms') as command:
            m.install_kernels('new',['k'],[('new','k','installed')],lambda *args:checked.append(args))
        command.assert_not_called();self.assertEqual(checked,[('new','k')])

    def test_all_builds_finish_before_any_replacement(self):
        calls=[]
        def command(*args):
            calls.append(args)
            if args[0]=='build' and args[-1]=='lts': raise RuntimeError('LTS build failed')
        with patch.object(m,'dkms',command):
            with self.assertRaises(RuntimeError):
                m.install_kernels('new',['main','lts'],[('old','main','installed')],lambda *a:None)
        self.assertEqual([c[0] for c in calls],['build','build'])

    def test_failed_install_restores_old_version(self):
        calls=[]
        def command(*args):
            calls.append(args)
            if args[0]=='install' and args[4]=='new': raise RuntimeError('install failed')
        with patch.object(m,'dkms',command),patch.object(m,'run',return_value=''):
            with self.assertRaises(RuntimeError):
                m.install_kernels('new',['k'],[('new','k','built'),('old','k','installed')],lambda *a:None)
        self.assertEqual([(c[0],c[4]) for c in calls],[('uninstall','old'),('install','new'),('install','old')])

    def test_existing_source_is_not_silently_overwritten(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'dkms.conf').write_bytes(b'wrong')
            with self.assertRaises(RuntimeError): m.check_source(p,{Path('dkms.conf'):b'expected'})
            self.assertEqual((p/'dkms.conf').read_bytes(),b'wrong')

if __name__=='__main__':unittest.main()
