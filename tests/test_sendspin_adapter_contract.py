#!/usr/bin/env python3
"""Dependency-free contract test for the Sendspin adapter companion (UI side).

Runs in the default UI suite with no pinned SDK, no cross toolchain and no
network. It pins the properties the C99 daemon relies on:

  * the adapter's CMake entry point is offline-only and player/PCM-only;
  * it is an explicit *opt-in* target — nothing here joins the default `make`;
  * the CMake header and AGENTS.md are *cross-file consistent* about the C++20
    companion: the header may only claim AGENTS.md documents the exception when
    AGENTS.md actually does, and while the instruction update is pending it is
    recorded OUTSTANDING rather than claimed as approved;
  * the heavy pinned-SDK integration is a separately selected lane.

If the companion's shape drifts, this fails deliberately.
"""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

UI = Path(__file__).resolve().parents[1]
CMAKELISTS = UI / "src/adapter/sendspin/CMakeLists.txt"
FIXTURE = UI / "tests/test_sendspin_sdk.cpp"
MAKEFILE = UI / "Makefile"
RUN_TESTS = UI / "tests/run_tests.sh"
AGENTS = UI / "AGENTS.md"

# --- Task 3: checked mirror of the frozen Platform LE_AUDIO_SINK/1 header -----
# The Platform header is the single source of truth; this UI header is a pure
# copy and must never be hand-maintained. The default lane proves identity
# against a pinned digest with no sibling checkout; the opt-in lane, selected
# with the existing SENDSPIN_PLATFORM_DIR convention, additionally compares the
# real Platform file byte-for-byte (a set-but-wrong source fails, never skips).
MIRROR = UI / "src/adapter/sendspin/audio_sink_protocol.h"
PLATFORM_ENV = "SENDSPIN_PLATFORM_DIR"
PLATFORM_REL = "tools/mt8163-arm32/airplay/audio_sink_protocol.h"
# sha256 of the authoritative Platform audio_sink_protocol.h (LE_AUDIO_SINK/1).
EXPECTED_SHA256 = "8f7f265e35c8743dc3dca9ab4e43a90917994cb714b4ea9d1d88249e450e0c26"

# A C99 syntax-only probe: negative array bounds are a compile-time assert, so a
# missing header, a non-C99 construct or a drifted ABI constant fails the build.
C99_PROBE = r"""
#include "audio_sink_protocol.h"

#define ASSERT_C(name, cond) typedef char assert_##name[(cond) ? 1 : -1]

/* Common header and datagram bounds. */
ASSERT_C(header_bytes, LE_AUDIO_SINK_HEADER_BYTES == 16u);
ASSERT_C(max_datagram, LE_AUDIO_SINK_MAX_DATAGRAM_BYTES == 16384u);
ASSERT_C(max_payload, LE_AUDIO_SINK_MAX_PAYLOAD_BYTES == 16368u);

/* Canonical 48 kHz S16_LE stereo geometry. */
ASSERT_C(rate, LE_AUDIO_SINK_OUTPUT_RATE == 48000u);
ASSERT_C(channels, LE_AUDIO_SINK_OUTPUT_CHANNELS == 2u);
ASSERT_C(bytes_per_frame, LE_AUDIO_SINK_BYTES_PER_FRAME == 4u);
ASSERT_C(period_frames, LE_AUDIO_SINK_PERIOD_FRAMES == 2048u);
ASSERT_C(max_capacity, LE_AUDIO_SINK_MAX_CAPACITY_FRAMES == 4096u);
ASSERT_C(max_data_frames, LE_AUDIO_SINK_MAX_DATA_FRAMES == 2048u);

/* Explicit per-message payload lengths (excluding the 16-byte header). */
ASSERT_C(open, LE_AUDIO_SINK_OPEN_PAYLOAD_BYTES == 20u);
ASSERT_C(open_ack, LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES == 36u);
ASSERT_C(data_prefix, LE_AUDIO_SINK_DATA_PREFIX_BYTES == 32u);
ASSERT_C(credit, LE_AUDIO_SINK_CREDIT_PAYLOAD_BYTES == 32u);
ASSERT_C(progress_req, LE_AUDIO_SINK_PROGRESS_REQ_PAYLOAD_BYTES == 8u);
ASSERT_C(progress, LE_AUDIO_SINK_PROGRESS_PAYLOAD_BYTES == 48u);
ASSERT_C(finish, LE_AUDIO_SINK_FINISH_PAYLOAD_BYTES == 24u);
ASSERT_C(finish_ack, LE_AUDIO_SINK_FINISH_ACK_PAYLOAD_BYTES == 28u);
ASSERT_C(reset, LE_AUDIO_SINK_RESET_PAYLOAD_BYTES == 16u);
ASSERT_C(reset_ack, LE_AUDIO_SINK_RESET_ACK_PAYLOAD_BYTES == 32u);
ASSERT_C(error, LE_AUDIO_SINK_ERROR_PAYLOAD_BYTES == 20u);

/* Opcodes and success status. */
ASSERT_C(open_type, LE_AUDIO_SINK_TYPE_OPEN == 0x01);
ASSERT_C(data_type, LE_AUDIO_SINK_TYPE_DATA == 0x02);
ASSERT_C(progress_req_type, LE_AUDIO_SINK_TYPE_PROGRESS_REQ == 0x03);
ASSERT_C(finish_type, LE_AUDIO_SINK_TYPE_FINISH == 0x04);
ASSERT_C(cancel_type, LE_AUDIO_SINK_TYPE_CANCEL == 0x05);
ASSERT_C(reset_type, LE_AUDIO_SINK_TYPE_RESET == 0x06);
ASSERT_C(open_ack_type, LE_AUDIO_SINK_TYPE_OPEN_ACK == 0x81);
ASSERT_C(credit_type, LE_AUDIO_SINK_TYPE_CREDIT == 0x82);
ASSERT_C(progress_type, LE_AUDIO_SINK_TYPE_PROGRESS == 0x83);
ASSERT_C(finish_ack_type, LE_AUDIO_SINK_TYPE_FINISH_ACK == 0x84);
ASSERT_C(reset_ack_type, LE_AUDIO_SINK_TYPE_RESET_ACK == 0x85);
ASSERT_C(error_type, LE_AUDIO_SINK_TYPE_ERROR == 0x86);
ASSERT_C(ok, LE_AUDIO_SINK_OK == 0);

int main(void)
{
    uint8_t buf[LE_AUDIO_SINK_HEADER_BYTES];
    struct le_audio_sink_header h;
    uint64_t cursor;
    int rc;

    h.version = 1; h.type = LE_AUDIO_SINK_TYPE_OPEN; h.flags = 0; h.length = 0;
    le_audio_sink_put_u16(buf, 0u);
    le_audio_sink_put_u32(buf, 0u);
    le_audio_sink_put_u64(buf, 0u);
    le_audio_sink_encode_header(buf, LE_AUDIO_SINK_TYPE_OPEN, 0u);
    (void)le_audio_sink_get_u16(buf);
    (void)le_audio_sink_get_u32(buf);
    (void)le_audio_sink_get_u64(buf);
    rc = le_audio_sink_decode_header(buf, LE_AUDIO_SINK_HEADER_BYTES, &h);
    if (rc != LE_AUDIO_SINK_OK)
        return 1;
    rc = le_audio_sink_cursor_add(0u, 1u, &cursor);
    if (rc != LE_AUDIO_SINK_OK)
        return 2;
    (void)cursor;
    return 0;
}
"""


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def assert_bytes_identical(case: unittest.TestCase, mirror: bytes, source: bytes,
                           label: str) -> None:
    """Assert two byte strings are identical, reporting the first divergence."""
    if mirror == source:
        return
    limit = min(len(mirror), len(source))
    for i in range(limit):
        if mirror[i] != source[i]:
            case.fail(
                f"{label}: first byte differs at offset {i}: "
                f"mirror=0x{mirror[i]:02x} source=0x{source[i]:02x} "
                f"(mirror {len(mirror)} B, source {len(source)} B)")
    case.fail(
        f"{label}: length differs, mirror {len(mirror)} B vs source {len(source)} B")


def resolve_platform_source():
    """Return the explicit Platform header path, or None when unset/blank.

    A non-empty SENDSPIN_PLATFORM_DIR always yields a path (even if it does not
    exist) so a set-but-wrong source is compared and fails, never skipped.
    """
    raw = os.environ.get(PLATFORM_ENV)
    if raw is None or raw.strip() == "":
        return None
    return Path(raw.strip()) / PLATFORM_REL


def _find_c_compiler():
    for name in (os.environ.get("CC", ""), "cc", "gcc", "clang"):
        if name and shutil.which(name):
            return shutil.which(name)
    return None

# A positive claim that AGENTS.md documents the policy exception. The current
# header writes "AGENTS.md rule 10 documents only a C++17 …" (no match); an
# approved header writes "AGENTS.md documents the C++20 exception" (match).
_APPROVAL_CLAIM = re.compile(r"AGENTS\.md\s+documents", re.IGNORECASE)


def assert_policy_consistency(case: unittest.TestCase, cmake_text: str, agents_text: str) -> None:
    """Require the CMakeLists and AGENTS.md to agree about the C++20 companion.

    This asserts a *cross-file consistency* property, never AGENTS.md's transient
    content, so the mandated approved update keeps the suite green:

    * if the header explicitly claims AGENTS.md documents the exception, AGENTS.md
      must actually document C++20 (otherwise it is a false approved-policy claim);
    * otherwise the header must record the update as pending (OUTSTANDING), not
      silently assume an approval that has not landed.
    """
    if _APPROVAL_CLAIM.search(cmake_text):
        case.assertIn(
            "C++20", agents_text,
            "CMakeLists.txt claims AGENTS.md documents the C++20 exception, but "
            "AGENTS.md does not document C++20 (false approved-policy claim)")
    else:
        case.assertIn(
            "OUTSTANDING", cmake_text,
            "CMakeLists.txt neither claims an approved C++20 policy nor records it "
            "as OUTSTANDING")


class AdapterCmakeContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = CMAKELISTS.read_text(encoding="utf-8")

    def test_offline_only_no_network_fetch(self) -> None:
        self.assertIn("FETCHCONTENT_FULLY_DISCONNECTED ON", self.text)
        for override in ("ARDUINOJSON", "MICRO_FLAC", "IXWEBSOCKET", "NOISE_C"):
            self.assertIn(f"FETCHCONTENT_SOURCE_DIR_{override}", self.text)

    def test_all_pinned_sources_are_required(self) -> None:
        for var in ("SENDSPIN_SDK_DIR", "SENDSPIN_ARDUINOJSON_DIR", "SENDSPIN_MICRO_FLAC_DIR",
                    "SENDSPIN_IXWEBSOCKET_DIR", "SENDSPIN_NOISE_C_DIR"):
            self.assertIn(var, self.text)
        self.assertIn("FATAL_ERROR", self.text)

    def test_player_and_pcm_only(self) -> None:
        self.assertIn("SENDSPIN_ENABLE_PLAYER     ON", self.text)
        self.assertIn("SENDSPIN_ENABLE_OPUS       OFF", self.text)
        for role in ("CONTROLLER", "METADATA", "COLOR", "ARTWORK", "VISUALIZER"):
            self.assertRegex(self.text, rf"SENDSPIN_ENABLE_{role}\s+OFF")
        self.assertIn("BUILD_EXAMPLES             OFF", self.text)
        self.assertIn("SENDSPIN_BUILD_TESTS       OFF", self.text)

    def test_policy_exception_is_cross_file_truthful(self) -> None:
        # Rule 10 documents the C++17 sherpa/onnx exception as baseline; the C++20
        # companion claim must stay consistent with whatever AGENTS.md documents.
        agents = AGENTS.read_text(encoding="utf-8")
        self.assertIn("C++17", agents)
        assert_policy_consistency(self, self.text, agents)


class PolicyConsistencyFixtureTests(unittest.TestCase):
    """Both allowed states (absent/pending and approved) plus the false claim.

    These fixtures keep the suite green on the mandated AGENTS.md update while
    still rejecting a header that claims an approval AGENTS.md never granted.
    """

    PENDING_CMAKE = (
        "# Sendspin companion\n"
        "# STATUS: the AGENTS.md instruction update remains OUTSTANDING.\n"
        "set(CMAKE_CXX_STANDARD 20)\n"
    )
    APPROVED_CMAKE = (
        "# Sendspin companion\n"
        "# AGENTS.md documents the C++20 exception for this companion.\n"
        "set(CMAKE_CXX_STANDARD 20)\n"
    )
    AGENTS_WITHOUT = "# Agent Guidelines\n- C++17 sherpa/onnx exception.\n"
    AGENTS_WITH = (
        "# Agent Guidelines\n"
        "- C++17 sherpa/onnx exception.\n"
        "- C++20 sendspin companion exception.\n"
    )

    def test_pending_absent_state_is_consistent(self) -> None:
        assert_policy_consistency(self, self.PENDING_CMAKE, self.AGENTS_WITHOUT)

    def test_pending_header_with_landed_policy_is_allowed(self) -> None:
        # The approved AGENTS.md line may land before the CMake header is refreshed;
        # a pending header that does not claim approval still passes.
        assert_policy_consistency(self, self.PENDING_CMAKE, self.AGENTS_WITH)

    def test_approved_state_is_consistent(self) -> None:
        assert_policy_consistency(self, self.APPROVED_CMAKE, self.AGENTS_WITH)

    def test_false_approved_claim_is_rejected(self) -> None:
        with self.assertRaises(AssertionError):
            assert_policy_consistency(self, self.APPROVED_CMAKE, self.AGENTS_WITHOUT)


class AdapterFixtureContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = FIXTURE.read_text(encoding="utf-8")

    def test_fixture_is_standalone_driver_of_the_real_sdk(self) -> None:
        self.assertIn('#include "lifecycle_test_fixtures.h"', self.text)
        self.assertIn("int main()", self.text)
        self.assertIn("PlayerRoleListener", self.text)

    def test_fixture_is_pcm_48k_stereo_s16_without_opus(self) -> None:
        self.assertIn("kSampleRate = 48000", self.text)
        self.assertIn("kBytesPerFrame = 4", self.text)
        self.assertNotRegex(self.text, r"\bopus\b", )


class OptInWiringTests(unittest.TestCase):
    def test_heavy_sdk_lane_is_opt_in(self) -> None:
        makefile = MAKEFILE.read_text(encoding="utf-8")
        self.assertIn("test-sendspin-contract:", makefile)
        self.assertIn("test-sendspin-sdk:", makefile)
        self.assertIn("SENDSPIN_PLATFORM_DIR", makefile)

    def test_default_runner_does_not_force_the_heavy_lane(self) -> None:
        runner = RUN_TESTS.read_text(encoding="utf-8")
        self.assertIn("test_sendspin_adapter_contract.py", runner)
        # The heavy SDK runner must be behind an explicit opt-in guard.
        self.assertRegex(runner, r"LIBREECHO_SENDSPIN_SDK")


class SinkProtocolMirrorTests(unittest.TestCase):
    """The UI copy of the frozen Platform LE_AUDIO_SINK/1 wire header.

    Task 3 shipped the Platform header as the single source of truth and its
    rejection tests; this lane pins the checked UI mirror. The default lane is
    standalone: identity comes from a pinned digest, so it needs no sibling
    checked checkout. When SENDSPIN_PLATFORM_DIR is set, the real Platform file is
    compared byte-for-byte and a missing or divergent source fails.
    """

    @classmethod
    def setUpClass(cls) -> None:
        if not MIRROR.is_file():
            raise AssertionError(
                f"checked Platform mirror is missing: {MIRROR}")

    def test_mirror_exists(self) -> None:
        self.assertTrue(
            MIRROR.is_file(),
            f"the checked Platform mirror is missing: {MIRROR}")

    def test_mirror_matches_the_frozen_digest(self) -> None:
        digest = sha256_hex(MIRROR.read_bytes())
        self.assertEqual(
            digest, EXPECTED_SHA256,
            "UI mirror does not match the frozen LE_AUDIO_SINK/1 digest "
            "(re-copy the Platform header byte-for-byte; never hand-edit it)")

    def test_mirror_pins_the_frozen_lengths_and_integer_serialization(self) -> None:
        text = MIRROR.read_text(encoding="utf-8")
        # Single source of truth identity and explicit common-header framing.
        self.assertIn('LE_AUDIO_SINK_PROTOCOL_ID "LE_AUDIO_SINK/1"', text)
        self.assertIn("#define LE_AUDIO_SINK_HEADER_BYTES 16u", text)
        self.assertIn("#define LE_AUDIO_SINK_MAX_DATAGRAM_BYTES 16384u", text)
        # Explicit per-message lengths (OPEN/DATA/CREDIT/PROGRESS/FINISH/RESET).
        for line in (
            "#define LE_AUDIO_SINK_OPEN_PAYLOAD_BYTES 20u",
            "#define LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES 36u",
            "#define LE_AUDIO_SINK_DATA_PREFIX_BYTES 32u",
            "#define LE_AUDIO_SINK_CREDIT_PAYLOAD_BYTES 32u",
            "#define LE_AUDIO_SINK_PROGRESS_REQ_PAYLOAD_BYTES 8u",
            "#define LE_AUDIO_SINK_PROGRESS_PAYLOAD_BYTES 48u",
            "#define LE_AUDIO_SINK_FINISH_PAYLOAD_BYTES 24u",
            "#define LE_AUDIO_SINK_FINISH_ACK_PAYLOAD_BYTES 28u",
            "#define LE_AUDIO_SINK_RESET_PAYLOAD_BYTES 16u",
            "#define LE_AUDIO_SINK_RESET_ACK_PAYLOAD_BYTES 32u",
            "#define LE_AUDIO_SINK_ERROR_PAYLOAD_BYTES 20u",
        ):
            self.assertIn(line, text)
        # Explicit little-endian codec, never raw structs/padding.
        for fn in ("le_audio_sink_put_u16", "le_audio_sink_put_u32",
                   "le_audio_sink_put_u64", "le_audio_sink_get_u16",
                   "le_audio_sink_get_u32", "le_audio_sink_get_u64",
                   "le_audio_sink_encode_header", "le_audio_sink_decode_header",
                   "le_audio_sink_cursor_add"):
            self.assertIn(fn, text, f"missing integer-serialization helper {fn}")

    def test_mirror_is_c99_compilable_with_frozen_abi_constants(self) -> None:
        compiler = _find_c_compiler()
        if compiler is None:
            self.fail(
                "no C99 compiler (cc/gcc/clang) found on PATH: the frozen ABI "
                "compile gate is mandatory and must never be silently skipped")
        with tempfile.TemporaryDirectory(prefix="sendspin-mirror-c99-") as tmp:
            probe = Path(tmp) / "probe.c"
            probe.write_text(C99_PROBE, encoding="utf-8")
            proc = subprocess.run(
                [compiler, "-std=c99", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                 "-fsyntax-only", "-I", str(MIRROR.parent), str(probe)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(
                proc.returncode, 0,
                "mirror is not C99-compilable with the frozen ABI constants:\n"
                + proc.stdout + proc.stderr)

    def test_explicit_platform_source_is_compared_not_skipped(self) -> None:
        source = resolve_platform_source()
        if source is None:
            self.skipTest(
                f"{PLATFORM_ENV} unset: standalone digest lane covers the mirror")
        self.assertTrue(
            source.is_file(),
            f"{PLATFORM_ENV} is set but {PLATFORM_REL} is missing under "
            f"{source.parent}; a provided source must be compared, not skipped")
        source_bytes = source.read_bytes()
        self.assertEqual(
            sha256_hex(source_bytes), EXPECTED_SHA256,
            "the provided Platform header is not the frozen LE_AUDIO_SINK/1 revision")
        assert_bytes_identical(
            self, MIRROR.read_bytes(), source_bytes,
            "UI mirror vs explicit Platform source")


class MissingCompilerFailClosedTests(unittest.TestCase):
    """A host without a C99 compiler must FAIL the ABI gate, never skip it.

    The frozen-ABI syntax probe is a mandatory, dependency-free gate, so an
    environment that cannot run it is a failure, not a hidden pass. This
    regression removes every compiler from discovery and drives the real gate
    directly (no full-suite subprocess recursion) to pin that contract.
    """

    def test_unavailable_compiler_discovery_fails_the_abi_gate(self) -> None:
        case = SinkProtocolMirrorTests(
            "test_mirror_is_c99_compilable_with_frozen_abi_constants")
        with mock.patch("shutil.which", return_value=None):
            self.assertIsNone(
                _find_c_compiler(),
                "compiler discovery must report nothing when none is on PATH")
            try:
                case.test_mirror_is_c99_compilable_with_frozen_abi_constants()
            except unittest.SkipTest as exc:
                self.fail(
                    "a missing C99 compiler skipped the mandatory ABI compile "
                    f"gate instead of failing: {exc}")
            except AssertionError as exc:
                self.assertRegex(
                    str(exc), r"C99", "the failure must name the C99 compiler")
                self.assertRegex(
                    str(exc), r"cc/gcc/clang",
                    "the failure must name the compiler candidates searched for")
            else:
                self.fail(
                    "missing C99 compiler did not fail the mandatory ABI gate")


class SinkProtocolDriftFixtureTests(unittest.TestCase):
    """The mirror comparison must reject any one-byte drift, not just differ."""

    def test_identical_bytes_are_accepted(self) -> None:
        assert_bytes_identical(self, b"\x01\x02\x03", b"\x01\x02\x03", "fixture")

    def test_single_byte_change_is_rejected(self) -> None:
        with self.assertRaises(AssertionError):
            assert_bytes_identical(self, b"\x01\x02\x03", b"\x01\x02\x04", "fixture")

    def test_truncated_mirror_is_rejected(self) -> None:
        with self.assertRaises(AssertionError):
            assert_bytes_identical(self, b"\x01\x02", b"\x01\x02\x03", "fixture")

    def test_extended_mirror_is_rejected(self) -> None:
        with self.assertRaises(AssertionError):
            assert_bytes_identical(self, b"\x01\x02\x03", b"\x01\x02", "fixture")

    def test_unset_or_blank_platform_dir_selects_the_digest_lane(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop(PLATFORM_ENV, None)
            self.assertIsNone(resolve_platform_source())
        with mock.patch.dict(os.environ, {PLATFORM_ENV: "   "}, clear=False):
            self.assertIsNone(resolve_platform_source())

    def test_set_but_missing_platform_dir_is_not_treated_as_absent(self) -> None:
        with mock.patch.dict(
                os.environ, {PLATFORM_ENV: "/nonexistent/sendspin-platform"},
                clear=False):
            source = resolve_platform_source()
            if source is None:
                self.fail(
                    "a set SENDSPIN_PLATFORM_DIR must be compared, not skipped")
            self.assertFalse(source.is_file())


class NativeBridgeCompanionTests(unittest.TestCase):
    """The C++20 engine-sink bridge is present, separate from the daemon, opt-in.

    It is a native companion artifact, not part of the C99 daemon: it must exist,
    be built only by its own lane, and never be pulled into the default build.
    """

    def test_engine_sink_headers_and_sources_exist(self) -> None:
        for rel in ("src/adapter/sendspin/engine_sink.h",
                    "src/adapter/sendspin/engine_sink.cpp"):
            self.assertTrue((UI / rel).is_file(), f"missing native bridge source: {rel}")
        header = (UI / "src/adapter/sendspin/engine_sink.h").read_text(encoding="utf-8")
        self.assertIn("audio_sink_protocol.h", header,
                      "the bridge must bind to the frozen LE_AUDIO_SINK/1 header")
        self.assertIn("C++20", header)

    def test_native_bridge_tests_exist(self) -> None:
        for rel in ("tests/test_sendspin_sink.cpp",
                    "tests/test_sendspin_lifecycle.cpp"):
            self.assertTrue((UI / rel).is_file(), f"missing native bridge test: {rel}")

    def test_sink_lane_is_opt_in_and_needs_the_real_engine(self) -> None:
        makefile = MAKEFILE.read_text(encoding="utf-8")
        self.assertIn("test-sendspin-sink:", makefile)
        self.assertIn("test-sendspin-sink-sanitize:", makefile)
        self.assertIn("SENDPIN_AIRPLAY", makefile)
        self.assertIn("audio_sink.c", makefile)
        self.assertIn("SENDSPIN_PLATFORM_DIR", makefile)
        runner = RUN_TESTS.read_text(encoding="utf-8")
        self.assertIn("LIBREECHO_SENDPIN_SINK", runner)

    def test_bridge_is_not_linked_into_the_c99_daemon(self) -> None:
        makefile = MAKEFILE.read_text(encoding="utf-8")
        prefix = makefile.split("test-sendspin-sink:", 1)[0]
        self.assertNotIn(
            "engine_sink", prefix,
            "the C++20 bridge must not be pulled into the default daemon build")


if __name__ == "__main__":
    unittest.main()
