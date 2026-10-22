"""Boundary tests for the native loader; uses tiny synthetic x86-64 images."""
from paths import ROOT, EXE
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest


def package(code, relocs=(), names=(), capabilities=None):
    image = code.ljust(4096, b'\0')
    header = struct.pack('<8sQQQQQ', b'BBPROBE1' if capabilities is None else b'BBPROBE2', len(image), 0, 1, len(relocs), len(names))
    if capabilities is not None:
        header += struct.pack('<Q', capabilities)
    segment = struct.pack('<QQQ', 0, len(image), 5)
    return (header + segment + b''.join(n.encode().ljust(128, b'\0') for n in names)
            + b''.join(struct.pack('<QQqq', *r) for r in relocs) + image)


def native_package(name='fixture-native', binding_address=4112, binding_kind=1,
                   lib_flags=5, init=b'\x31\xc0\xc3', native=b'\xc3', metadata=None):
    # Main calls an import then tail-jumps to the terminal diagnostic import.
    code = b'\x48\x83\xec\x08\xff\x15\x16\0\0\0\x48\x83\xc4\x08\xff\x25\x14\0\0\0'
    image = code.ljust(4096,b'\0') + init.ljust(16,b'\0') + native
    image = image.ljust(12288,b'\0')
    header=struct.pack('<8s6Q',b'BBPROBE3',len(image),0,3,2,2,1)
    meta=metadata or (4096,8192,4096,8192,32,4,1,256)
    return (header + struct.pack('<8Q',*meta) + struct.pack('<3Q',0,binding_address,binding_kind)
            + struct.pack('<9Q',0,4096,5,4096,4096,lib_flags,8192,4096,6)
            + name.encode().ljust(128,b'\0') + b'after-native'.ljust(128,b'\0')
            + struct.pack('<8Q',32,1,0,0,40,1,1,0) + image)


@unittest.skipUnless(EXE.exists(), 'build native probe first')
class LoaderTests(unittest.TestCase):
    def test_invalid_content_profile_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'content.bin'
            for data in (b'BBCONT01',struct.pack('<8s5I',b'BBCONT01',2,0,0,0,0),
                         struct.pack('<8s5I',b'BBCONT01',3,0,0,0,0)+b'extra'):
                with self.subTest(data=data):
                    path.write_bytes(data)
                    r=self.run_image(package(b'\xc3'),'--content-profile',str(path))
                    self.assertEqual(r.returncode,1,r.stdout+r.stderr)
                    self.assertIn('content profile',r.stderr)

    def run_image(self, data, *options):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'boot.bin'
            path.write_bytes(data)
            return subprocess.run([str(EXE.resolve()), str(path), '--cpu-only', *options],
                                  capture_output=True, text=True, timeout=5)

    def test_original_instruction_reaches_named_import(self):
        # jmp [rip+2]; two padding bytes; relocated function pointer at +8
        r = self.run_image(package(b'\xff\x25\x02\0\0\0\x90\x90', [(8, 1, 0, 0)], ['fixture-import']))
        self.assertEqual(r.returncode, 20, r.stderr)
        self.assertIn('first unsupported PS4 import: fixture-import', r.stdout)

    def test_native_initializer_and_export_return(self):
        r=self.run_image(native_package())
        self.assertEqual(r.returncode,20,r.stdout+r.stderr)
        self.assertIn('Module 0 initializer returned 0',r.stdout)
        self.assertIn('first unsupported PS4 import: after-native',r.stdout)

    def test_host_contract_takes_priority_over_native_export(self):
        r=self.run_image(native_package(name='bzQExy189ZI#q#q',native=b'\x0f\x0b'))
        self.assertEqual(r.returncode,20,r.stdout+r.stderr)
        self.assertIn('_init_env returned',r.stdout)
        self.assertIn('first unsupported PS4 import: after-native',r.stdout)

    def test_strict_mode_skips_native_initialization_and_binding(self):
        r=self.run_image(native_package(init=b'\x0f\x0b'), '--strict-imports')
        self.assertEqual(r.returncode,20,r.stdout+r.stderr)
        self.assertIn('first unsupported PS4 import: fixture-native',r.stdout)
        self.assertNotIn('Starting native libc',r.stdout)

    def test_native_metadata_and_bindings_are_validated(self):
        for kwargs,message in [
            ({'binding_address':12288},'invalid native binding'),
            ({'binding_kind':2},'native export kind/range mismatch'),
            ({'lib_flags':4},'native function is not executable'),
            ({'metadata':(4096,8192,4096,8192,32,33,1,256)},'invalid linked module metadata'),
            ({'metadata':(4096,8192,4096,8192,32,4,1,12280)},'unmapped procparam'),
        ]:
            with self.subTest(kwargs=kwargs):
                r=self.run_image(native_package(**kwargs))
                self.assertEqual(r.returncode,1,r.stdout+r.stderr)
                self.assertIn(message,r.stderr)

    def test_failed_native_initializer_does_not_enter_game(self):
        r=self.run_image(native_package(init=b'\xb8\x01\0\0\0\xc3'))
        self.assertEqual(r.returncode,1,r.stdout+r.stderr)
        self.assertIn('module initializer failed',r.stderr)
        self.assertNotIn('Entering original',r.stdout)

    def test_base_relative_address_is_relocated(self):
        # jump via a relative relocation to code at +16, then to an import at +24
        code = b'\xff\x25\x02\0\0\0\x90\x90' + b'\0'*8 + b'\xff\x25\x02\0\0\0\x90\x90'
        r = self.run_image(package(code, [(8, 0, 16, 0), (24, 1, 0, 0)], ['after-relative-jump']))
        self.assertEqual(r.returncode, 20, r.stderr)
        self.assertIn('after-relative-jump', r.stdout)

    def test_out_of_bounds_relocation_is_rejected(self):
        r = self.run_image(package(b'\xc3', [(4092, 0, 0, 0)]))
        self.assertEqual(r.returncode, 1)
        self.assertIn('bad relocation', r.stderr)

    def test_truncated_image_is_rejected(self):
        r = self.run_image(package(b'\xc3')[:-1])
        self.assertEqual(r.returncode, 1)
        self.assertIn('incorrect memory image size', r.stderr)

    def test_illegal_instruction_reports_guest_offset(self):
        r = self.run_image(package(b'\x0f\x0b'))
        self.assertEqual(r.returncode, 132)
        self.assertIn('at guest offset 0x0,', r.stderr)

    def run_spinning_image(self, *options, env=None, timeout=30):
        """A guest that loops forever (jmp $): only a timeout or a crash ends it."""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'boot.bin'
            path.write_bytes(package(b'\xeb\xfe'))
            return subprocess.run([str(EXE.resolve()), str(path), '--cpu-only', *options],
                                  capture_output=True, text=True, timeout=timeout, cwd=tmp,
                                  env=None if env is None else {**os.environ, **env})

    @unittest.skipUnless(os.name == 'nt', 'Windows exit path')
    def test_timeout_terminates_the_process(self):
        r = self.run_spinning_image('--timeout', '1')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('[TIMEOUT]', r.stdout)

    @unittest.skipUnless(os.name == 'nt', 'Windows crash handler')
    def test_blocked_crash_handler_still_ends_the_process(self):
        # The handler blocks after the dump (BB_CRASH_TEST=hang): the reaper thread must
        # terminate the process a few seconds later instead of leaving it alive.
        r = self.run_spinning_image('--timeout', '0', env={'BB_CRASH_TEST': 'hang:1'})
        self.assertEqual(r.returncode, 139, r.stdout + r.stderr)
        self.assertIn('Fault at RIP', r.stderr)

    def test_runtime_returns_from_verified_init_env(self):
        # Align stack, call _init_env through +32, then tail-jump to unknown +40.
        code = b'\x48\x83\xec\x08\xff\x15\x16\0\0\0\x48\x83\xc4\x08\xff\x25\x14\0\0\0'
        data = package(code, [(32,1,0,0),(40,1,1,0)], ['bzQExy189ZI#q#q','after-init'], 1)
        r = self.run_image(data)
        self.assertEqual(r.returncode,20,r.stderr)
        self.assertIn('first unsupported PS4 import: after-init',r.stdout)
        self.assertIn('_init_env=1',r.stdout)
        strict = self.run_image(data,'--strict-imports')
        self.assertEqual(strict.returncode,20,strict.stderr)
        self.assertIn('first unsupported PS4 import: bzQExy189ZI#q#q',strict.stdout)

    def test_unverified_runtime_stays_disabled(self):
        r = self.run_image(package(b'\xff\x25\x02\0\0\0\x90\x90',[(8,1,0,0)],['bzQExy189ZI#q#q'],0))
        self.assertEqual(r.returncode,20,r.stderr)
        self.assertIn('_init_env=0',r.stdout)

    def run_in_tmp(self, data, *options, env=None, timeout=30):
        """Runs an image in a temporary folder; returns the result and its crash log (or '')."""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'boot.bin'
            path.write_bytes(data)
            r = subprocess.run([str(EXE.resolve()), str(path), '--cpu-only', *options],
                               capture_output=True, text=True, timeout=timeout, cwd=tmp,
                               env=None if env is None else {**os.environ, **env})
            logs = sorted((Path(tmp) / 'out').glob('crash_*.log'))
            return r, logs[-1].read_text(errors='replace') if logs else ''

    @unittest.skipUnless(os.name == 'nt', 'Windows crash handler')
    def test_guest_null_dereference_has_its_own_exit_code(self):
        r, dump = self.run_in_tmp(package(b'\x8b\x04\x25\x08\0\0\0'))  # mov eax, [8]
        self.assertEqual(r.returncode, 140, r.stdout + r.stderr)
        self.assertIn('Exit code:      140 (guest NULL dereference', dump)
        self.assertRegex(dump, r'Instruction:    mov eax, \[0x0*8\]')
        self.assertIn('--- GUEST MEMORY ---', dump)
        self.assertIn('Memory failures: pool=0 allocate=0', dump)
        self.assertIn('--- GUEST ASSERTIONS ---\nNone this run.', dump)

    @unittest.skipUnless(os.name == 'nt', 'Windows crash handler')
    def test_other_guest_fault_keeps_139(self):
        # movabs rax, 0x7ff0dead0000; mov eax, [rax]
        r, dump = self.run_in_tmp(package(b'\x48\xb8' + struct.pack('<Q', 0x7ff0dead0000) + b'\x8b\x00'))
        self.assertEqual(r.returncode, 139, r.stdout + r.stderr)
        self.assertIn('Exit code:      139\n', dump)
        self.assertRegex(dump, r'Instruction:    mov eax, \[rax\]')

    def raise_image(self):
        # edi=0xa0020007, esi=5; call sceKernelDebugRaiseException through +32, then tail-jump
        # to the unknown import at +40.
        code = (b'\xbf\x07\x00\x02\xa0\xbe\x05\0\0\0'
                b'\x48\x83\xec\x08\xff\x15\x0c\0\0\0\x48\x83\xc4\x08\xff\x25\x0a\0\0\0')
        return package(code, [(32, 1, 0, 0), (40, 1, 1, 0)], ['OMDRKKAZ8I4#p#J', 'after-raise'], 1)

    def test_debug_exception_is_logged_and_the_game_goes_on(self):
        r, _ = self.run_in_tmp(self.raise_image(), env={'BB_GAME_ASSERT_STOP': ''})
        self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
        self.assertIn('raised debug exception 0xa0020007 (argument 0x5) from +0x14 ', r.stderr)
        self.assertIn('going on as on a PS4', r.stderr)
        self.assertIn('first unsupported PS4 import: after-raise', r.stdout)

    def test_debug_exception_stops_with_assert_stop(self):
        r, _ = self.run_in_tmp(self.raise_image(), env={'BB_GAME_ASSERT_STOP': '1'})
        self.assertEqual(r.returncode, 142, r.stdout + r.stderr)
        self.assertIn('stopping (BB_GAME_ASSERT_STOP=1)', r.stderr)
        self.assertNotIn('after-raise', r.stdout)

    def panic_image(self):
        # The game's panic printer prologue at +0x100 (body: return the line), called with
        # rdi="Foo.cpp", esi=42, rdx="panic %d", rcx=1, r8=2; the result goes on as the first
        # argument of the unknown import at +0x40.
        code = (b'\x48\x8d\x3d\x59\0\0\0' b'\xbe\x2a\0\0\0' b'\x48\x8d\x15\x5d\0\0\0'
                b'\xb9\x01\0\0\0' b'\x41\xb8\x02\0\0\0' b'\x31\xc0' b'\x48\x83\xec\x08'
                b'\xe8\xd7\0\0\0' b'\x48\x83\xc4\x08' b'\x89\xc7' b'\xff\x25\x0b\0\0\0')
        image = bytearray(code.ljust(0x100, b'\0'))
        image[0x60:0x68] = b'Foo.cpp\0'
        image[0x70:0x79] = b'panic %d\0'
        image = bytes(image)
        image += (b'\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x81\xec\x00\x01\x00\x00'  # prologue
                  b'\x89\xf0' b'\x48\x81\xc4\x00\x01\x00\x00' b'\x5b\x41\x5c\x41\x5e\x41\x5f\x5d\xc3')
        return package(image, [(0x40, 1, 0, 0)], ['after-panic'])

    def test_panic_printer_hook_records_and_preserves_arguments(self):
        plain, _ = self.run_in_tmp(self.panic_image(), env={'BB_GAME_PANIC_HOOK': ''})
        self.assertEqual(plain.returncode, 20, plain.stdout + plain.stderr)
        self.assertIn('first argument: 0x2a', plain.stdout)
        self.assertNotIn('[GAME PANIC]', plain.stderr)
        r, _ = self.run_in_tmp(self.panic_image(), env={'BB_GAME_PANIC_HOOK': '0x100'})
        self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
        self.assertIn("panic printer (+0x100) is recorded", r.stdout)
        self.assertIn('[GAME PANIC] Foo.cpp(42): "panic %d" (0x1, 0x2) from +0x29', r.stderr)
        self.assertIn('first argument: 0x2a', r.stdout)  # the printer still got its arguments

    def test_failed_allocation_is_recorded(self):
        # sceKernelAllocateDirectMemory(low 0, high 0, 64 KiB, ...): no space in an empty range.
        code = (b'\x31\xff\x31\xf6\xba\x00\x00\x01\x00\x31\xc9\x45\x31\xc0'
                b'\x48\x83\xec\x18\x49\x89\xe1\xff\x15\x15\0\0\0\x48\x83\xc4\x18\xff\x25\x13\0\0\0')
        data = package(code, [(48, 1, 0, 0), (56, 1, 1, 0)], ['rTXw65xmLIA#p#J', 'after-alloc'], 1)
        r, _ = self.run_in_tmp(data)
        self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
        self.assertIn('Runtime: memory failure [direct allocate (range)] size=65536', r.stderr)
        self.assertIn('Memory failures: pool=0 allocate=1 direct-map=0', r.stdout)

    def test_unknown_capabilities_rejected(self):
        r = self.run_image(package(b'\xc3',capabilities=2))
        self.assertEqual(r.returncode,1)
        self.assertIn('unknown runtime capabilities',r.stderr)


if __name__ == '__main__':
    unittest.main()
