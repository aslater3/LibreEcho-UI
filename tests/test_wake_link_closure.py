#!/usr/bin/env python3
"""Exercise the shipped wake link recipe with the pinned ORT archive layout.

This is a native linker regression, not an ARM32/ONNX inference test. Only the
three target-architecture flags are translated; the production recipe, archive
selection, static linking and unresolved-symbol checks execute unchanged.
The pinned ORT 8f0278c7 external-dependency/common CMake targets do not use nsync.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CORE = ("session", "optimizer", "providers", "graph", "framework", "common",
        "mlas", "util", "flatbuffers", "lora")
DEPS = ("onnx-build/libonnx.a", "onnx-build/libonnx_proto.a",
        "protobuf-build/libprotobuf-lite.a", "flatbuffers-build/libflatbuffers.a")


class WakeLinkClosure(unittest.TestCase):
    def command(self, args, **kwargs):
        return subprocess.run(args, cwd=ROOT, text=True, capture_output=True,
                              timeout=20, **kwargs)

    def test_pinned_layout_links_and_required_archive_remains_mandatory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ort, speex = root / "ort", root / "speex"
            archives = [ort / f"libonnxruntime_{part}.a" for part in CORE]
            archives += [ort / "_deps" / part for part in DEPS]
            archives += [ort / "_deps/abseil_cpp-build/absl/libabsl_fixture.a",
                         speex / "lib/libspeexdsp.a", root / "libre2.a"]
            for archive in archives:
                archive.parent.mkdir(parents=True, exist_ok=True)
                self.command(["ar", "rcs", str(archive)], check=True)
            # A real unresolved reference must be satisfied from the cached
            # ONNX dependency. There is deliberately no nsync file or stub.
            main = root / "main.c"
            main.write_text("int dependency_probe(void);\n"
                            "int main(void) { return dependency_probe() != 42; }\n")
            dependency = root / "dependency.c"
            dependency.write_text("int dependency_probe(void) { return 42; }\n")
            obj, dep_obj = root / "main.o", root / "dependency.o"
            for source, output in ((main, obj), (dependency, dep_obj)):
                self.command(["cc", "-c", str(source), "-o", str(output)], check=True)
            required = ort / "_deps/onnx-build/libonnx_proto.a"
            self.command(["ar", "rcs", str(required), str(dep_obj)], check=True)
            compiler = shutil.which("g++")
            self.assertIsNotNone(compiler)
            wrapper = root / "native-g++"
            wrapper.write_text("#!/usr/bin/env python3\nimport os, sys\n"
                               "flags = ('-march=', '-mfpu=', '-mfloat-abi=')\n"
                               f"os.execv({compiler!r}, [{compiler!r}] + "
                               "[a for a in sys.argv[1:] if not a.startswith(flags)])\n")
            wrapper.chmod(0o755)
            output = root / "build/libreecho-waked-onnx-arm32"
            output.parent.mkdir()
            args = ["make", "--no-print-directory", "-s", str(output),
                    f"BUILD={output.parent}", f"CROSS_COMPILE={root}/native-",
                    f"WAKE_DAEMON_ARM_OBJECTS={obj}", f"WAKE_ORT_BUILD={ort}",
                    f"ARM_SPEEX_PREFIX={speex}", f"RE2_ARCHIVE={root}/libre2.a",
                    "SOURCE_COMMIT=fixture", "SOURCE_DIRTY=0", "SOURCE_DIGEST=fixture"]
            result = self.command(args)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.command([str(output)], check=True)
            self.assertFalse((ort / "_deps/nsync-build").exists())
            output.unlink()
            required.unlink()
            result = self.command(args)
            self.assertNotEqual(result.returncode, 0, "missing required archive accepted")
            self.assertIn("libonnx_proto.a", result.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
