import importlib.util
import io
import pathlib
import struct
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))


def load_tool(name: str):
    path = ROOT / "tools" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class MmioReadTests(unittest.TestCase):
    def test_identity_profile_is_small_and_allowlisted(self):
        module = load_tool("mmio_read")
        offsets = [offset for offset, _name in module.PROFILES["identity"]]
        self.assertEqual(offsets, [0x20, 0x24, 0x28, 0x2C, 0x2218, 0x2234])
        self.assertTrue(all(offset % 4 == 0 for offset in offsets))
        self.assertTrue(all(offset + 4 <= module.EXPECTED_BAR0_SIZE for offset in offsets))

    def test_expected_endpoint_profile(self):
        module = load_tool("mmio_read")
        self.assertEqual(module.EXPECTED_VENDOR, 0x1A00)
        self.assertEqual(module.EXPECTED_DEVICE, 0x0002)
        self.assertEqual(module.EXPECTED_BAR0_SIZE, 65536)

    def test_resource_layout_covers_eleven_registers_for_eight_dsps(self):
        module = load_tool("mmio_read")
        words = module.PROFILES["resource-layout"]
        self.assertEqual(len(words), 88)
        self.assertEqual(len({offset for offset, _name in words}), 88)
        self.assertEqual(module.dsp_register_base(0), 0x0000)
        self.assertEqual(module.dsp_register_base(4), 0x4000)
        self.assertEqual(module.dsp_register_base(7), 0x5800)
        self.assertTrue(all(offset % 4 == 0 for offset, _name in words))
        self.assertTrue(
            all(offset + 4 <= module.EXPECTED_BAR0_SIZE for offset, _name in words)
        )

    def test_uio_profile_matches_octo_and_allowlist(self):
        module = load_tool("uio_mmio_read")
        self.assertEqual(module.EXPECTED_VENDOR, 0x1A00)
        self.assertEqual(module.EXPECTED_DEVICE, 0x0002)
        self.assertEqual(module.EXPECTED_SUBDEVICE, 0x0005)
        self.assertEqual(
            [offset for offset, _name in module.IDENTITY_WORDS],
            [0x20, 0x24, 0x28, 0x2C, 0x2218, 0x2234],
        )

    def test_dsp_status_profile_has_eight_distinct_slots(self):
        module = load_tool("uio_mmio_read")
        self.assertEqual(
            [module.dsp_bank(index) for index in range(8)],
            [0x2000, 0x2080, 0x2100, 0x2180, 0x6000, 0x6080, 0x6100, 0x6180],
        )
        self.assertEqual(
            [module.dsp_poll(index) for index in range(8)],
            [0x1A4, 0x9A4, 0x11A4, 0x19A4, 0x41A4, 0x49A4, 0x51A4, 0x59A4],
        )
        offsets = [offset for offset, _name in module.DSP_STATUS_WORDS]
        self.assertEqual(len(offsets), 26)
        self.assertEqual(len(offsets), len(set(offsets)))

        self.assertTrue(all(offset % 4 == 0 for offset in offsets))
        self.assertTrue(
            all(offset + 4 <= module.EXPECTED_BAR0_SIZE for offset in offsets)
        )

    def test_ring_register_profile_covers_two_rings_for_eight_dsps(self):
        module = load_tool("uio_mmio_read")
        offsets = [offset for offset, _name in module.RING_REGISTER_WORDS]
        self.assertEqual(len(offsets), 2 + 8 * 2 * 11)
        self.assertEqual(len(offsets), len(set(offsets)))
        self.assertTrue(all(offset % 4 == 0 for offset in offsets))
        self.assertTrue(
            all(offset + 4 <= module.EXPECTED_BAR0_SIZE for offset in offsets)
        )

    def test_dma_control_profile_excludes_interrupt_registers(self):
        module = load_tool("uio_mmio_read")
        offsets = [offset for offset, _name in module.DMA_CONTROL_WORDS]
        self.assertIn(0x2200, offsets)
        self.assertNotIn(0x2204, offsets)
        self.assertNotIn(0x2208, offsets)
        self.assertNotIn(0x2264, offsets)
        self.assertNotIn(0x2268, offsets)
        self.assertEqual(len(offsets), len(set(offsets)))


class OfficialDriverInspectorTests(unittest.TestCase):
    def test_signatures_are_nonempty_and_have_unique_addresses(self):
        module = load_tool("inspect_official_driver")
        addresses = [address for address, _bytes, _meaning in module.SIGNATURES]
        self.assertGreaterEqual(len(addresses), 45)
        self.assertEqual(len(addresses), len(set(addresses)))
        self.assertTrue(all(expected for _address, expected, _meaning in module.SIGNATURES))
        self.assertEqual(len(module.KNOWN_SHA256), 64)
        self.assertEqual(
            [stage["stage"] for stage in module.STARTUP_SEQUENCE],
            list(range(1, 8)),
        )

    def test_unknown_driver_hash_is_refused(self):
        module = load_tool("inspect_official_driver")
        with tempfile.NamedTemporaryFile() as candidate:
            candidate.write(b"not a driver")
            candidate.flush()
            with self.assertRaisesRegex(ValueError, "driver hash is not the analyzed"):
                module.inspect(pathlib.Path(candidate.name))


class FrameworkDriverInspectorTests(unittest.TestCase):
    def test_signatures_symbols_and_tables_are_explicit(self):
        module = load_tool("inspect_framework_driver")
        addresses = [address for address, _bytes, _meaning in module.SIGNATURES]
        self.assertGreaterEqual(len(addresses), 21)
        self.assertEqual(len(addresses), len(set(addresses)))
        self.assertTrue(all(expected for _address, expected, _meaning in module.SIGNATURES))
        self.assertEqual(len(module.KNOWN_SHA256), 64)
        self.assertEqual(len(module.PUBLIC_COMMIT), 40)
        self.assertIn("__ZN11CPcieDevice13HardResetDSPsEv", module.SYMBOLS)
        self.assertEqual(len(module.SWITCH_TABLE), 13 * 4)
        self.assertEqual(module.PROPERTY_SIZES[6], 44)
        self.assertEqual(
            next(item for item in module.PROPERTY_PATHS if item["id"] == 6)["register_offsets"],
            [
                "0x010", "0x018", "0x014", "0x01c", "0x184", "0x18c",
                "0x188", "0x190", "0x198", "0x194", "0x19c",
            ],
        )

    def test_unknown_framework_driver_is_refused(self):
        module = load_tool("inspect_framework_driver")
        with tempfile.NamedTemporaryFile() as candidate:
            candidate.write(b"not the framework driver")
            candidate.flush()
            with self.assertRaisesRegex(ValueError, "driver hash is not the analyzed"):
                module.inspect(pathlib.Path(candidate.name))

    def test_macho_parser_rejects_invalid_input(self):
        module = load_tool("inspect_framework_driver")
        with self.assertRaisesRegex(ValueError, "truncated Mach-O"):
            module.MachOImage(b"short")


class StartupProfileTests(unittest.TestCase):
    def test_observed_octo_omits_audio_extension_and_compresses_vectors(self):
        module = load_tool("decode_startup_profile")
        profile = module.decode(0xA012DC0D, 0x00300811)
        self.assertEqual(profile["dsp_count"], 8)
        self.assertTrue(profile["compressed_interrupt_mapping"])
        self.assertFalse(profile["extended_interrupt_bank"])
        self.assertFalse(profile["audio_extension"])
        self.assertFalse(profile["shared_4_mib_dma_applicable"])
        self.assertEqual(profile["callback_shadow"], "0x00000000cccccccc")
        self.assertEqual(profile["dsp0_query_shadow"], "0x00000000cccccccf")
        self.assertEqual(profile["dma_shadow_sequence"][-1], "0x000001ff")

    def test_each_fifth_logical_vector_is_unmapped(self):
        module = load_tool("decode_startup_profile")
        profile = module.decode(0xA012DC0D, 0x00300811)
        mapping = {item["logical"]: item["physical"] for item in profile["interrupt_mapping"]}
        self.assertEqual([mapping[index] for index in range(10)], [0, 1, 2, 3, None, 4, 5, 6, 7, None])


class MsiTableInspectorTests(unittest.TestCase):
    def test_string_pool_supports_long_strings(self):
        module = load_tool("inspect_msi_tables")
        long_value = "x" * 70000
        pool = struct.pack("<HHHHI", 1252, 0, 0, 1, len(long_value))
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            (directory / "!_StringPool").write_bytes(pool)
            (directory / "!_StringData").write_bytes(long_value.encode("ascii"))
            self.assertEqual(module.load_string_pool(directory), ["", long_value])

    def test_msi_integer_bias_and_null(self):
        module = load_tool("inspect_msi_tables")
        self.assertIsNone(module.decode_integer(0, 2))
        self.assertEqual(module.decode_integer(0x8001, 2), 1)
        self.assertEqual(module.decode_integer(0x80000011, 4), 17)


class UadContainerInspectorTests(unittest.TestCase):
    def test_hbut_header_and_payload_length(self):
        module = load_tool("inspect_uad_container")
        payload = bytes(range(16))
        words = [
            int.from_bytes(b"HBUT", "little"),
            0x616E1720,
            0x2A,
            0xA012DC0D,
            0x030F8598,
            0x03000005,
            len(payload) // 4,
            0xFFFFFFFF,
        ] + list(range(8, 16))
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "FirmwareUpdateOcto.bin"
            path.write_bytes(struct.pack("<16I", *words) + payload)
            result = module.inspect(path)
        self.assertEqual(result["magic"], "HBUT")
        self.assertEqual(result["header_size"], 64)
        self.assertEqual(result["compatibility_id"], "0xa012dc0d")
        self.assertEqual(result["declared_payload_dwords"], 4)
        self.assertTrue(result["declared_size_matches_file"])

    def test_unknown_magic_is_refused(self):
        module = load_tool("inspect_uad_container")
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "unknown.bin"
            path.write_bytes(b"NOPE" + bytes(60))
            with self.assertRaisesRegex(ValueError, "unsupported container magic"):
                module.inspect(path)

    def test_truncated_container_is_refused(self):
        module = load_tool("inspect_uad_container")
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "short.bin"
            path.write_bytes(b"HBUT")
            with self.assertRaisesRegex(ValueError, "shorter than"):
                module.inspect(path)


class FirmwareInventoryTests(unittest.TestCase):
    def test_timestamp_decoder_rejects_legacy_counter_and_decodes_build_time(self):
        module = load_tool("inventory_uad_firmware")
        self.assertIsNone(module.timestamp_utc(7))
        self.assertEqual(module.timestamp_utc(0x616E1720), "2021-10-19T00:53:52Z")

    def test_sha256_tail_hypotheses_are_explicit(self):
        module = load_tool("inventory_uad_firmware")
        prefix = bytes(32)
        payload = b"synthetic payload"
        digest = __import__("hashlib").sha256(payload).digest()
        self.assertEqual(
            module.sha256_tail_matches(prefix + digest + payload), ["payload"]
        )


class ContainerComparisonTests(unittest.TestCase):
    def test_comparison_reports_equal_payload_structure(self):
        module = load_tool("compare_uad_containers")
        header = struct.pack(
            "<16I",
            int.from_bytes(b"HBUT", "little"),
            0x616E1720,
            42,
            0xA012DC0D,
            1,
            2,
            4,
            0xFFFFFFFF,
            *range(8),
        )
        with tempfile.TemporaryDirectory() as directory:
            left = pathlib.Path(directory) / "left.bin"
            right = pathlib.Path(directory) / "right.bin"
            left.write_bytes(header + bytes(range(16)))
            right.write_bytes(header + bytes(range(16)))
            result = module.compare(left, right)
        self.assertEqual(result["equal_byte_fraction"], 1.0)
        self.assertEqual(result["longest_equal_run"], 16)
        self.assertEqual(result["equal_16_byte_blocks"], 1)
        self.assertEqual(result["equal_header_words"], list(range(16)))


class PcieLoaderInspectorTests(unittest.TestCase):
    def test_loader_signatures_are_hash_locked_and_unique(self):
        module = load_tool("inspect_pcie_loader")
        addresses = [address for address, _bytes, _meaning in module.SIGNATURES]
        self.assertGreaterEqual(len(addresses), 12)
        self.assertEqual(len(addresses), len(set(addresses)))
        self.assertEqual(len(module.KNOWN_SHA256), 64)
        self.assertTrue(all(expected for _address, expected, _meaning in module.SIGNATURES))
        self.assertTrue(
            any("0x0be0deaf" in meaning for _address, _expected, meaning in module.SIGNATURES)
        )

    def test_unknown_loader_driver_is_refused(self):
        module = load_tool("inspect_pcie_loader")
        with tempfile.NamedTemporaryFile() as candidate:
            candidate.write(b"not the PCIe driver")
            candidate.flush()
            with self.assertRaisesRegex(ValueError, "driver hash mismatch"):
                module.inspect(pathlib.Path(candidate.name))


class QemuVfioTraceAnalyzerTests(unittest.TestCase):
    def test_trace_summary_recovers_bar_milestones_and_config_access(self):
        module = load_tool("analyze_qemu_vfio_trace")
        trace = "\n".join(
            [
                "vfio_pci_read_config  (0000:03:00.0, @0x0, len=0x4) 0x21a00",
                "vfio_pci_write_config  (0000:03:00.0, @0x4, 0x7, len=0x2)",
                "vfio_region_write  (0000:03:00.0:region0+0x221c, 0x1, 4)",
                "vfio_region_write  (0000:03:00.0:region0+0x221c, 0x0, 4)",
                "vfio_region_write  (0000:03:00.0:region0+0x1a8, 0xbe0deaf, 4)",
                "vfio_region_write  (0000:03:00.0:region0+0x2200, 0x1ff, 4)",
                "vfio_region_read  (0000:03:00.0:region0+0x2218, 4) = 0xa012dc0d",
            ]
        )
        writes = io.StringIO()
        result = module.summarize(io.StringIO(trace), writes)
        self.assertTrue(result["milestones"]["bar0_access_seen"])
        self.assertTrue(result["milestones"]["ordinary_hard_reset_assert_seen"])
        self.assertTrue(result["milestones"]["ordinary_hard_reset_deassert_seen"])
        self.assertTrue(result["milestones"]["firmware_transition_magic_seen"])
        self.assertTrue(result["milestones"]["all_eight_dma_enable_seen"])
        self.assertEqual(result["config_registers"][0]["offset"], "0x000")
        self.assertIn("hard_reset", writes.getvalue())

    def test_ring_and_dsp_labels_cover_all_eight_engines(self):
        module = load_tool("analyze_qemu_vfio_trace")
        self.assertEqual(module.label_bar0_offset(0x2000), "dsp0_command_page0_low")
        self.assertEqual(module.label_bar0_offset(0x2040), "dsp0_response_page0_low")
        self.assertEqual(module.label_bar0_offset(0x6000), "dsp4_command_page0_low")
        self.assertEqual(module.label_bar0_offset(0x61C0), "dsp7_response_page0_low")
        self.assertEqual(module.label_bar0_offset(0x59A4), "dsp7_ready")


class Uad2RingDumpAnalyzerTests(unittest.TestCase):
    def test_command_dump_reports_only_nonzero_entries(self):
        module = load_tool("analyze_uad2_ring_dump")
        data = bytearray(module.RING_SIZE)
        struct.pack_into("<4I", data, 16, 0x00100002, 0x12345678, 0, 0)
        result = module.analyze(bytes(data), "command")
        entries = result["rings"][0]["nonzero_entries"]
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["index"], 1)
        self.assertEqual(entries[0]["kind"], "inline-command")

    def test_dma_descriptor_combines_64_bit_address(self):
        module = load_tool("analyze_uad2_ring_dump")
        data = bytearray(module.RING_SIZE)
        struct.pack_into("<4I", data, 0, 0x80000400, 0, 0x89ABC000, 0x12)
        result = module.analyze(bytes(data), "response", dsp=3)
        entry = result["rings"][0]["nonzero_entries"][0]
        self.assertEqual(entry["kind"], "dma-descriptor")
        self.assertEqual(entry["address"], "0x0000001289abc000")
        self.assertEqual(result["rings"][0]["dsp"], 3)

    def test_all_dsp_layout_has_sixteen_rings(self):
        module = load_tool("analyze_uad2_ring_dump")
        result = module.analyze(bytes(module.ALL_DSP_RING_SIZE), "all-dsps")
        self.assertEqual(len(result["rings"]), 16)
        self.assertEqual(result["rings"][-1]["dsp"], 7)
        self.assertEqual(result["rings"][-1]["ring"], "response")


class ResponseBoundaryCaptureTests(unittest.TestCase):
    def test_response_ring_base_recovers_four_64_bit_addresses(self):
        module = load_tool("../lab/windows/capture_response_boundary")
        words = {}
        for page in range(4):
            low_offset = 0x2040 + page * 8
            module.update_response_page_words(
                f"vfio_region_write  (0000:03:00.0:region0+0x{low_offset:x}, "
                f"0x{0x12345000 + page * 0x1000:x}, 4)",
                words,
            )
            module.update_response_page_words(
                f"vfio_region_write  (0000:03:00.0:region0+0x{low_offset + 4:x}, "
                "0x2, 4)",
                words,
            )
        self.assertEqual(
            module.response_page_addresses(words),
            [0x212345000, 0x212346000, 0x212347000, 0x212348000],
        )

    def test_consumed_index_selects_previous_descriptor(self):
        module = load_tool("../lab/windows/capture_response_boundary")
        page = bytearray(4096)
        struct.pack_into("<4I", page, 2 * 16, 0x80000302, 0, 0xABCDF000, 1)
        descriptor = module.descriptor_for_consumed_index(bytes(page), 3)
        self.assertEqual(descriptor["descriptor_index"], 2)
        self.assertEqual(descriptor["target_address"], 0x1ABCDF000)
        self.assertTrue(descriptor["descriptor_valid"])

    def test_hmp_filename_quotes_absolute_path(self):
        module = load_tool("../lab/windows/capture_response_boundary")
        self.assertEqual(
            module.hmp_filename(pathlib.Path("/tmp/uad boundary/ring.bin")),
            '"/tmp/uad boundary/ring.bin"',
        )

    def test_hmp_filename_rejects_monitor_injection(self):
        module = load_tool("../lab/windows/capture_response_boundary")
        with self.assertRaisesRegex(ValueError, "unsupported character"):
            module.hmp_filename(pathlib.Path('/tmp/bad"path'))


class ResponseSequenceCaptureTests(unittest.TestCase):
    def test_trace_state_tracks_bases_indices_and_transition(self):
        module = load_tool("../lab/windows/capture_response_sequence")
        state = module.TraceState()
        for base in (module.COMMAND_BASE, module.RESPONSE_BASE):
            for page in range(4):
                low = base + page * 8
                module.update_trace_state(
                    f"vfio_region_write  (0000:03:00.0:region0+0x{low:x}, "
                    f"0x{0x10000000 + page * 0x1000:x}, 4)",
                    state,
                )
                module.update_trace_state(
                    f"vfio_region_write  (0000:03:00.0:region0+0x{low + 4:x}, "
                    "0x1, 4)",
                    state,
                )
        module.update_trace_state(
            "vfio_region_write  (0000:03:00.0:region0+0x2024, 0x12, 4)", state
        )
        self.assertIsNone(
            module.update_trace_state(
                "vfio_region_read  (0000:03:00.0:region0+0x2068, 4) = 0x8",
                state,
            )
        )
        self.assertEqual(
            module.update_trace_state(
                "vfio_region_read  (0000:03:00.0:region0+0x2068, 4) = 0x9",
                state,
            ),
            ("response-read", 8, 9),
        )
        self.assertEqual(state.command_host_index, 0x12)
        self.assertEqual(
            module.page_addresses(state.response_page_words, module.RESPONSE_BASE),
            [0x110000000, 0x110001000, 0x110002000, 0x110003000],
        )

    def test_advanced_indices_handles_wrap(self):
        module = load_tool("../lab/windows/capture_response_sequence")
        self.assertEqual(module.advanced_indices(1022, 2), [1022, 1023, 0, 1])
        self.assertEqual(module.advanced_indices(7, 7), [])

    def test_descriptor_page_selection_and_dma_fields(self):
        module = load_tool("../lab/windows/capture_response_sequence")
        pages = [bytearray(4096) for _ in range(4)]
        struct.pack_into(
            "<4I", pages[2], 3 * 16, 0x80000073, 0, 0x23456000, 0x1
        )
        words = module.descriptor_words([bytes(page) for page in pages], 515)
        descriptor = module.dma_descriptor(words)
        self.assertTrue(descriptor["valid"])
        self.assertEqual(descriptor["length_dwords"], 0x73)
        self.assertEqual(descriptor["length_bytes"], 0x1CC)
        self.assertEqual(descriptor["address"], 0x123456000)

    def test_response_classifier_covers_known_forms(self):
        module = load_tool("../lab/windows/capture_response_sequence")
        page = bytearray(4096)
        struct.pack_into("<4I", page, 0, 0x80070004, 0, 0x120, 0x10099)
        self.assertEqual(
            module.classify_response(bytes(page))["kind"],
            "bill-intermediate-success",
        )
        self.assertEqual(module.classify_response(bytes(4096))["kind"], "all-zero")

    def test_command_host_write_reports_transition(self):
        module = load_tool("../lab/windows/capture_response_sequence")
        state = module.TraceState(command_host_index=7)
        self.assertEqual(
            module.update_trace_state(
                "vfio_region_write  (0000:03:00.0:region0+0x2024, 0x9, 4)",
                state,
            ),
            ("command-host", 7, 9),
        )

    def test_command_metadata_finds_bill_after_resource_envelope(self):
        module = load_tool("../lab/windows/capture_response_sequence")
        pages = [bytearray(4096) for _ in range(4)]
        target = bytearray(32)
        struct.pack_into("<2I4s4I", target, 0, 0x00010008, 0x4000,
                         b"Bill", 0x120, 0x02000000, 4, 0)
        struct.pack_into("<4I", pages[0], 0, 0x80000008, 0, 0x1000, 0)
        old_save = module.save_memory
        module.save_memory = lambda host, port, address, size, path: bytes(target)
        try:
            result = module.command_metadata(
                [bytes(page) for page in pages], [0], pathlib.Path("/tmp"),
                "127.0.0.1", 4444,
            )[0]
        finally:
            module.save_memory = old_save
        self.assertEqual(result["bill_metadata"]["offset_bytes"], 8)
        self.assertEqual(result["bill_metadata"]["resource_id"], "0x00000120")
        self.assertEqual(result["bill_metadata"]["payload_form"], 0)
        self.assertEqual(result["bill_metadata"]["dsp_generation"], 2)
        self.assertEqual(result["bill_metadata"]["replacement_dwords"], 0)
        self.assertEqual(
            result["resource_envelope"]["allocation_offset_dwords"],
            "0x00004000",
        )

    def test_response_sampling_schedule_stays_below_loader_timeout(self):
        module = load_tool("../lab/windows/capture_response_sequence")
        self.assertEqual(module.RESPONSE_SAMPLE_DELAYS_MS, (0, 5, 100))
        self.assertLess(
            max(module.RESPONSE_SAMPLE_DELAYS_MS),
            module.RESOURCE_COMPLETION_WAIT_MS,
        )

    def test_hmp_filename_requires_absolute_safe_path(self):
        module = load_tool("../lab/windows/capture_response_sequence")
        with self.assertRaisesRegex(ValueError, "absolute"):
            module.hmp_filename(pathlib.Path("relative.bin"))
        with self.assertRaisesRegex(ValueError, "unsupported"):
            module.hmp_filename(pathlib.Path('/tmp/bad"path'))


class ResponseAnalyzerTests(unittest.TestCase):
    def test_authorization_table_response_is_classified(self):
        module = load_tool("analyze_uad2_response")
        words = [0] * 1024
        words[0] = 0x80030302
        words[1] = 0x12345678
        words[2:770] = [
            0x80000000,
            0x81000000,
            0x82000000,
            0x83000000,
        ] * 192
        result = module.analyze(struct.pack("<1024I", *words))
        self.assertTrue(result["authorization_table_candidate"])
        self.assertEqual(result["response_kind"], "authorization-table")
        self.assertEqual(result["declared_dwords"], 770)
        self.assertEqual(result["body_dwords"], 768)
        self.assertEqual(result["body_value_counts"]["0x80000000"], 192)
        self.assertTrue(result["trailing_all_zero"])

    def test_invalid_declared_length_is_rejected(self):
        module = load_tool("analyze_uad2_response")
        data = struct.pack("<1024I", 0x80030001, *([0] * 1023))
        with self.assertRaisesRegex(ValueError, "outside the page"):
            module.analyze(data)

    def test_non_page_capture_is_rejected(self):
        module = load_tool("analyze_uad2_response")
        with self.assertRaisesRegex(ValueError, "exactly 4096"):
            module.analyze(bytes(32))

    def test_bill_intermediate_success_is_classified(self):
        module = load_tool("analyze_uad2_response")
        words = [0] * 1024
        words[:4] = [0x80070004, 0, 0x00000120, 0x00010099]
        result = module.analyze(struct.pack("<1024I", *words))
        self.assertEqual(result["response_kind"], "bill-intermediate-success")
        self.assertTrue(result["bill_intermediate_success"])
        self.assertEqual(result["resource_id"], "0x00000120")
        self.assertEqual(result["resource_command_word"], "0x00010099")

    def test_all_zero_resource_response_is_classified(self):
        module = load_tool("analyze_uad2_response")
        result = module.analyze(bytes(4096))
        self.assertEqual(result["response_kind"], "all-zero")
        self.assertFalse(result["valid_bit"])
        self.assertEqual(result["declared_dwords"], 0)

    def test_bill_final_response_is_classified(self):
        module = load_tool("analyze_uad2_response")
        words = [0] * 1024
        words[:4] = [0x80020044, 0, 0, 0xF0060003]
        result = module.analyze(struct.pack("<1024I", *words))
        self.assertEqual(result["response_kind"], "bill-final")
        self.assertTrue(result["bill_final_response"])
        self.assertEqual(result["bill_final_status_code"], 3)


class UpdaterStateInspectorTests(unittest.TestCase):
    def test_signatures_are_hash_locked_and_unique(self):
        module = load_tool("inspect_updater_state")
        for signatures in (module.PERFMON_SIGNATURES, module.CLIENT_SIGNATURES):
            addresses = [address for address, _bytes, _meaning in signatures]
            self.assertEqual(len(addresses), len(set(addresses)))
            self.assertTrue(all(expected for _address, expected, _meaning in signatures))
        self.assertEqual(len(module.PERFMON_SHA256), 64)
        self.assertEqual(len(module.CLIENT_SHA256), 64)

    def test_unknown_updater_binary_is_refused(self):
        module = load_tool("inspect_updater_state")
        with tempfile.NamedTemporaryFile() as candidate:
            candidate.write(b"not an updater")
            candidate.flush()
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                module.verify(
                    pathlib.Path(candidate.name),
                    module.PERFMON_SHA256,
                    module.PERFMON_SIGNATURES,
                )

    def test_recovered_system_info_fields_fit_the_record(self):
        module = load_tool("inspect_updater_state")
        fields = [
            (0x20, 4),
            (0x24, 4),
            (0x28, 4),
            (0x2C, 4),
            (0x58, 1),
            (0xA0, 4),
        ]
        self.assertTrue(all(offset + size <= 168 for offset, size in fields))

    def test_firmware_dispatch_has_no_invented_bracketing(self):
        module = load_tool("inspect_updater_state")
        self.assertTrue(
            any(
                "FBUT, GBUT, and HBUT" in meaning
                for _address, _expected, meaning in module.PERFMON_SIGNATURES
            )
        )
        self.assertTrue(
            any(
                "calls LoadBinFile directly" in meaning
                for _address, _expected, meaning in module.PERFMON_SIGNATURES
            )
        )


class SystemInfoPathInspectorTests(unittest.TestCase):
    def test_signatures_are_hash_locked_and_unique(self):
        module = load_tool("inspect_system_info_path")
        for signatures in (module.SYSTEM_SIGNATURES, module.PCIE_SIGNATURES):
            addresses = [address for address, _bytes, _meaning in signatures]
            self.assertEqual(len(addresses), len(set(addresses)))
            self.assertTrue(all(expected for _address, expected, _meaning in signatures))
        self.assertEqual(len(module.SYSTEM_SHA256), 64)
        self.assertEqual(len(module.PCIE_SHA256), 64)

    def test_unknown_binary_is_refused(self):
        module = load_tool("inspect_system_info_path")
        with tempfile.NamedTemporaryFile() as candidate:
            candidate.write(b"not a driver")
            candidate.flush()
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                module.verify(
                    pathlib.Path(candidate.name),
                    module.SYSTEM_SHA256,
                    module.SYSTEM_SIGNATURES,
                )


class AuthorizationStateInspectorTests(unittest.TestCase):
    def test_signatures_are_hash_locked_and_unique(self):
        module = load_tool("inspect_authorization_states")
        for signatures in (
            module.PERFMON_SIGNATURES,
            module.SYSTEM_SIGNATURES,
            module.PCIE_SIGNATURES,
        ):
            addresses = [address for address, _bytes, _meaning in signatures]
            self.assertEqual(len(addresses), len(set(addresses)))
            self.assertTrue(all(expected for _address, expected, _meaning in signatures))
        self.assertEqual(len(module.PERFMON_SHA256), 64)
        self.assertEqual(len(module.SYSTEM_SHA256), 64)
        self.assertEqual(len(module.PCIE_SHA256), 64)

    def test_exact_wire_state_mapping(self):
        module = load_tool("inspect_authorization_states")
        self.assertEqual(module.decode_wire_value(0), (3, "demo expired", 0))
        self.assertEqual(
            module.decode_wire_value(0x80000000),
            (2, "demo not started", None),
        )
        self.assertEqual(module.decode_wire_value(0x81000000), (0, "authorized", None))
        self.assertEqual(module.decode_wire_value(0x82000000), (0, "authorized", None))
        self.assertEqual(
            module.decode_wire_value(0x83000000),
            (4, "authorization update required", None),
        )
        self.assertEqual(module.decode_wire_value(0x00001000), (1, "demo active", 1))
        self.assertEqual(module.decode_wire_value(0x00001001), (1, "demo active", 2))

    def test_observed_table_counts_collapse_to_display_states(self):
        module = load_tool("inspect_authorization_states")
        values = (
            [0x80000000] * 203
            + [0x81000000] * 2
            + [0x82000000] * 24
            + [0x83000000] * 539
        )
        summary = module.summarize_wire_values(values)
        self.assertEqual(summary["entry_count"], 768)
        self.assertEqual(
            summary["display_state_counts"],
            {
                "authorization update required": 539,
                "authorized": 26,
                "demo not started": 203,
            },
        )

    def test_unknown_binary_is_refused(self):
        module = load_tool("inspect_authorization_states")
        with tempfile.NamedTemporaryFile() as candidate:
            candidate.write(b"not an analyzed UAD binary")
            candidate.flush()
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                module.verify(
                    pathlib.Path(candidate.name),
                    module.PCIE_SHA256,
                    module.PCIE_SIGNATURES,
                )


class PublicBillStatusInspectorTests(unittest.TestCase):
    def test_signatures_and_error_mapping_are_hash_locked(self):
        module = load_tool("inspect_public_bill_status")
        self.assertEqual(len(module.KNOWN_SHA256), 64)
        addresses = [address for address, _expected, _meaning in module.SIGNATURES]
        self.assertEqual(len(addresses), len(set(addresses)))
        self.assertTrue(all(expected for _address, expected, _meaning in module.SIGNATURES))
        self.assertEqual(module.EXPLICIT_STATUS_TO_HOST_ERROR[5], -55)
        self.assertEqual(module.EXPLICIT_STATUS_TO_HOST_ERROR[13], -60)
        self.assertEqual(module.host_error(0x0005), -55)
        self.assertEqual(module.host_error(0x000D), -60)
        self.assertEqual(module.host_error(0x0123), -57)

    def test_unknown_binary_is_refused(self):
        module = load_tool("inspect_public_bill_status")
        with tempfile.NamedTemporaryFile() as candidate:
            candidate.write(b"not the analyzed public kext")
            candidate.flush()
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                module.inspect(pathlib.Path(candidate.name))


class BillLoaderInspectorTests(unittest.TestCase):
    def test_signatures_and_completion_mapping_are_explicit(self):
        module = load_tool("inspect_bill_loader")
        addresses = [address for address, _bytes, _meaning in module.SIGNATURES]
        self.assertGreaterEqual(len(addresses), 16)
        self.assertEqual(len(addresses), len(set(addresses)))
        self.assertTrue(all(expected for _address, expected, _meaning in module.SIGNATURES))
        self.assertEqual(
            sorted(module.FINAL_STATUS_TO_HOST_ERROR),
            ["0x0001", "0x0002", "0x0003", "0x0004", "0x0005", "0x0008", "0x0009"],
        )

    def test_unknown_system_driver_is_refused(self):
        module = load_tool("inspect_bill_loader")
        with tempfile.NamedTemporaryFile() as candidate:
            candidate.write(b"not UAD2System")
            candidate.flush()
            with self.assertRaisesRegex(ValueError, "driver hash mismatch"):
                module.inspect(pathlib.Path(candidate.name))


class BillContainerInspectorTests(unittest.TestCase):
    def test_parser_reproduces_official_tail_transform(self):
        module = load_tool("inspect_bill_container")
        resource_id = 0x020000C2
        body = bytes(range(32)) + bytes(12)
        data = struct.pack(
            "<4s4I", b"Bill", resource_id, 0x02010001, len(body), 3
        ) + body
        result = module.parse(data)
        expected_tail = module.replacement_stream(resource_id, 3)

        self.assertEqual(result["resource_id"], "0x020000c2")
        self.assertEqual(result["dsp_generation"], 2)
        self.assertEqual(result["payload_form"], 1)
        self.assertTrue(result["tail_transform_applied"])
        self.assertEqual(result["preserved_bytes"], len(data) - 12)
        self.assertEqual(result["transformed"][-12:], expected_tail)
        self.assertFalse(result["input_tail_already_transformed"])

    def test_payload_form_zero_is_copied_unchanged(self):
        module = load_tool("inspect_bill_container")
        body = bytes(range(32)) + bytes(12)
        data = struct.pack(
            "<4s4I", b"Bill", 0x020000C2, 0x02000000, len(body), 3
        ) + body
        result = module.parse(data)

        self.assertEqual(result["payload_form"], 0)
        self.assertFalse(result["tail_transform_applied"])
        self.assertIsNone(result["input_tail_already_transformed"])
        self.assertEqual(result["transformed"], data)

    def test_replacement_stream_starts_with_complemented_id_big_endian(self):
        module = load_tool("inspect_bill_container")
        stream = module.replacement_stream(0x020000C2, 2)
        self.assertEqual(stream[:4], bytes.fromhex("fdffff3d"))
        next_value = (((~0x020000C2) & 0xFFFFFFFF) * 0xBC8F) % 0x7FFFFFFF
        self.assertEqual(stream[4:8], next_value.to_bytes(4, "big"))

    def test_parser_rejects_inconsistent_size(self):
        module = load_tool("inspect_bill_container")
        data = struct.pack("<4s4I", b"Bill", 1, 0x02000000, 16, 1) + bytes(12)
        with self.assertRaisesRegex(ValueError, "declared body size"):
            module.parse(data)

    def test_resource_command_contains_exact_pool_header(self):
        module = load_tool("inspect_bill_container")
        body = bytes(range(32)) + bytes(12)
        data = struct.pack(
            "<4s4I", b"Bill", 0x020000C2, 0x02000000, len(body), 3
        ) + body

        low = module.build_command(data, 0x1234, "low-to-high")
        high = module.build_command(data, 0x5678, "high-to-low")
        self.assertEqual(struct.unpack_from("<2I", low), (0x00010012, 0x1234))
        self.assertEqual(struct.unpack_from("<2I", high), (0x00040012, 0x5678))
        self.assertEqual(low[8:], data)


class BillResourceScannerTests(unittest.TestCase):
    def test_scanner_finds_valid_embedded_resources_and_rejects_false_magic(self):
        module = load_tool("scan_bill_resources")
        body = bytes(range(32)) + bytes(12)
        resource = struct.pack(
            "<4s4I", b"Bill", 0x020000C2, 0x02000000, len(body), 3
        ) + body
        data = b"prefixBillfalse" + bytes(7) + resource + b"suffix"

        results = module.scan_bytes(data)

        self.assertEqual(len(results), 1)
        self.assertEqual(results[0]["offset"], data.index(resource))
        self.assertEqual(results[0]["resource_id"], "0x020000c2")
        self.assertEqual(results[0]["input_sha256"], __import__("hashlib").sha256(resource).hexdigest())

    def test_scanner_caps_untrusted_declared_size(self):
        module = load_tool("scan_bill_resources")
        hostile = struct.pack("<4s4I", b"Bill", 1, 0x02000001, 0xFFFFFFFF, 1)
        self.assertEqual(module.scan_bytes(hostile), [])


class BillResourceAnalyzerTests(unittest.TestCase):
    def test_analyzer_reports_prefix_sizes_and_rejects_direct_sha_hypotheses(self):
        module = load_tool("analyze_bill_resources")
        body = bytes(range(32)) + bytes(12)
        resource = struct.pack(
            "<4s4I", b"Bill", 0x020000C2, 0x02000000, len(body), 3
        ) + body
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "module.bin"
            path.write_bytes(resource + resource)
            result = module.analyze_files([path])

        self.assertEqual(result["resource_instances"], 2)
        self.assertEqual(result["unique_resource_sha256"], 1)
        self.assertEqual(result["duplicate_instances"], 1)
        self.assertEqual(
            result["distributions"]["opaque_prefix_bytes_after_header"], {"32": 2}
        )
        for hypothesis in result["direct_sha256_hypotheses"].values():
            self.assertEqual(hypothesis, {"eligible": 2, "matches": 0})
        self.assertEqual(result["schema"], 3)
        self.assertEqual(result["entropy_bits_per_byte"]["inner_core"]["count"], 1)
        self.assertEqual(
            result["standard_digest_subsequence_hypotheses"]["sha256_inner_core"],
            {"eligible": 1, "matches_anywhere_in_prefix": 0},
        )
        self.assertEqual(
            result["aligned_16_byte_block_tests"]["distinct_blocks_shared_by_multiple_unique_resources"],
            0,
        )
        self.assertEqual(
            result["standard_sharc_ldr_wire_body_scan"]["candidate_count"], 0
        )

    def test_standard_sharc_loader_scan_recognizes_both_word_endiannesses(self):
        module = load_tool("analyze_bill_resources")
        little = struct.pack("<III", 0x5, 5, 0x8C100) + bytes(30)
        big = struct.pack(">III", 0x2, 17, 0x8C105)

        little_result = module.scan_standard_sharc_ldr_headers(little)
        big_result = module.scan_standard_sharc_ldr_headers(big)

        self.assertEqual(
            little_result["candidates_by_endian_and_tag"]["little"]["INIT_L48"],
            1,
        )
        self.assertEqual(
            big_result["candidates_by_endian_and_tag"]["big"]["ZERO_L48"],
            1,
        )


class PluginAllocationCaptureTests(unittest.TestCase):
    def test_capture_parser_decodes_native_counts_and_scrubs_pointers(self):
        module = load_tool("inspect_plugin_alloc_capture")
        record = bytearray(module.RECORD_SIZE)
        struct.pack_into("<II", record, 0, module.RECORD_SIZE, 2)
        struct.pack_into("<QQ", record, 8, 0x7FF600001000, 0x7FF600001120)
        struct.pack_into("<I", record, 0x188, 1)
        struct.pack_into("<IIII", record, 0x18C, 1, 430, 0xFFFFFFFF, 0)
        struct.pack_into("<I", record, 0x98C, 1)
        struct.pack_into("<II", record, 0x990, 419, 4)
        capture = (
            module.CAPTURE_HEADER.pack(module.CAPTURE_MAGIC, 1, 1)
            + module.RECORD_ENVELOPE.pack(module.RECORD_SIZE, 0x7FF600000000, 0)
            + record
        )
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "allocation.bin"
            path.write_bytes(capture)
            result = module.inspect(path)

        decoded = result["records"][0]
        self.assertEqual(decoded["record_bytes"], 0xAF8)
        self.assertEqual(decoded["resource_relative_offsets"], [0, 0x120])
        self.assertEqual(decoded["memory_specs"][0]["word1"], 430)
        self.assertEqual(
            decoded["readbacks"][0],
            {"index": 0, "resource": 0, "dword_offset": 419, "dword_count": 4},
        )
        self.assertEqual(len(decoded["record_sha256_pointer_scrubbed"]), 64)

    def test_capture_proxy_forwards_the_original_record_unchanged(self):
        source = (
            ROOT / "tools" / "windows" / "uad2_alloc_capture_proxy.c"
        ).read_text()
        exports = (
            ROOT / "tools" / "windows" / "uad2_alloc_capture_proxy.def"
        ).read_text()
        self.assertIn("count < 32", source)
        self.assertIn("record_size < 0xac8 || record_size > 0x4000", source)
        self.assertIn("return function.typed(version, allocation, status);", source)
        self.assertIn("CreateUAD2PlugIn2=wrap_CreateUAD2PlugIn2 @8", exports)


class PublicBillCorpusAuditTests(unittest.TestCase):
    def test_comparison_reports_only_resource_id_high_byte_difference(self):
        module = load_tool("audit_public_bill_corpus")
        body = bytes(range(32)) + bytes(12)
        public = struct.pack(
            "<4s4I", b"Bill", 0x020000C2, 0x02000000, len(body), 3
        ) + body
        official = bytearray(public)
        official[7] = 0
        key = module._key(bytes(official))
        result = module.compare_arrays({"fixture": public}, {key: bytes(official)})[0]
        self.assertEqual(result["differing_byte_offsets"], [7])
        self.assertTrue(result["all_bytes_after_resource_id_equal"])

    def test_public_header_hash_is_locked(self):
        module = load_tool("audit_public_bill_corpus")
        self.assertEqual(len(module.PUBLIC_HEADER_SHA256), 64)
        self.assertEqual(len(module.PUBLIC_COMMIT), 40)


class Experiment011SourceTests(unittest.TestCase):
    def test_probe_stays_below_dma_and_command_boundary(self):
        source = (ROOT / "tools" / "vfio_official_ring_init.c").read_text()
        self.assertIn("#define PAGE_COUNT 8", source)
        self.assertNotIn("mmio_write32(bar, DMA_CONTROL", source)
        self.assertNotIn("0x2204", source)
        self.assertNotIn("0x2208", source)
        self.assertIn('\\"command_entries_submitted\\": 0', source)

    def test_command_ring_is_initialized_before_response_ring(self):
        source = (ROOT / "tools" / "vfio_official_ring_init.c").read_text()
        command = source.index("initialize_ring(bar, DSP0_CMD_BASE")
        response = source.index("initialize_ring(bar, DSP0_RESP_BASE")
        self.assertLess(command, response)


class Experiment012SourceTests(unittest.TestCase):
    def test_snapshot_has_no_dma_mapping_or_mmio_write_helper(self):
        source = (ROOT / "tools" / "vfio_capability_snapshot.c").read_text()
        self.assertNotIn("VFIO_IOMMU_MAP_DMA", source)
        self.assertNotIn("mmio_write32", source)
        self.assertIn('PROT_READ, MAP_SHARED', source)
        self.assertIn('\\"mmio_writes\\": 0', source)


class Experiment020SourceTests(unittest.TestCase):
    def test_resource_layout_snapshot_is_read_only_and_complete(self):
        source = (ROOT / "tools" / "vfio_resource_layout.c").read_text()
        self.assertIn("#define DSP_COUNT 8", source)
        self.assertIn("PROT_READ, MAP_SHARED", source)
        self.assertIn("dma_mappings", source)
        self.assertIn("mmio_writes", source)
        self.assertNotIn("VFIO_IOMMU_MAP_DMA", source)
        self.assertNotIn("mmio_write32", source)
        for offset in ("0x184", "0x188", "0x18c", "0x190", "0x194", "0x198", "0x19c"):
            self.assertIn(offset, source)


class Experiment021SourceTests(unittest.TestCase):
    def test_runtime_loader_is_exact_bounded_and_excludes_fpga_operation(self):
        source = (ROOT / "tools" / "vfio_runtime_load.c").read_text()
        wrapper = (ROOT / "tools" / "uad2-vfio-runtime-load.sh").read_text()
        self.assertIn("#define EXPECTED_FILE_SIZE 2558096U", source)
        self.assertIn("#define EXPECTED_FPGA_REVISION 0xa012dc0dU", source)
        self.assertIn("#define LOADER_COMMAND_BASE 0x00120000U", source)
        self.assertIn("#define LOADER_EXTENDED_FLAG 0x40000000U", source)
        self.assertIn("header_buffer[1] = loader_command[1]", source)
        self.assertIn("ring_entry(memory, 0, 0, 2), 2", source)
        self.assertIn("VFIO_IOMMU_MAP_DMA", source)
        self.assertIn("VFIO_IOMMU_UNMAP_DMA", source)
        self.assertIn("VFIO_DEVICE_RESET", source)
        self.assertNotIn("0x00130000", source)
        self.assertNotIn("0x6a", source.lower())
        self.assertIn(
            "f503787c0f253fc9713a47ae7e15adff242a6dde647dae7cb8ab6550ed976447",
            wrapper,
        )
        self.assertIn("UAD2_ALLOW_PERSISTENT_FIRMWARE_EXPERIMENT", wrapper)
        self.assertIn("YES_I_ACCEPT_CARD_FIRMWARE_RISK", wrapper)


class Experiment013SourceTests(unittest.TestCase):
    def test_full_start_maps_only_per_dsp_ring_pages(self):
        source = (ROOT / "tools" / "vfio_full_octo_start.c").read_text()
        self.assertIn("#define PAGE_COUNT (DSP_COUNT * RINGS_PER_DSP * PAGES_PER_RING)", source)
        self.assertIn("#define DSP_COUNT 8", source)
        self.assertIn("#define DMA_ALL_DSPS 0x000001ff", source)
        self.assertNotIn("write32(bar, 0x8000", source)
        self.assertNotIn("write32(bar, 0xa000", source)
        self.assertIn('\\"command_entries_submitted\\": 0', source)


    def test_per_dsp_reset_mode_uses_official_enable_and_pulse_bits(self):
        source = (ROOT / "tools" / "vfio_full_octo_start.c").read_text()
        self.assertIn('strcmp(argv[1], "--exercise-dsp-resets")', source)
        self.assertIn("1U << (dsp + 1)", source)
        self.assertIn("1U << (dsp + 9)", source)
        self.assertIn("DMA_ALL_DSPS & ~enable_bit", source)
        self.assertIn("reset_pass_mask == 0x1fe", source)


class Experiment014SourceTests(unittest.TestCase):
    def test_query_uses_full_octo_state_and_compressed_mask(self):
        source = (ROOT / "tools" / "vfio_full_query.c").read_text()
        self.assertIn("#define PAGE_COUNT 67", source)
        self.assertIn("#define DMA_ALL_DSPS 0x000001ff", source)
        self.assertIn("#define CALLBACK_SHADOW 0xcccccccc", source)
        self.assertIn("#define QUERY_SHADOW 0xcccccccf", source)
        self.assertIn("#define QUERY_COMMAND 0x00260001", source)
        self.assertNotIn("write32(bar, 0x8000", source)
        self.assertNotIn("write32(bar, 0xa000", source)
        self.assertIn("#define CONNECT_COMMAND 0x00230002", source)
        self.assertIn("#define CLOCK_COMMAND 0x00100002", source)
        self.assertIn("#define SEQUENCE_COMMAND 0x00270001", source)
        self.assertIn("#define SEQUENCE_RESPONSE_HEADER 0x800d0002", source)
        self.assertIn('strcmp(argv[1], "--connect")', source)

    def test_authenticated_bill_modes_are_bounded_and_target_specific(self):
        source = (ROOT / "tools" / "vfio_full_query.c").read_text()
        wrapper = (ROOT / "tools" / "uad2-vfio-full-query.sh").read_text()
        self.assertIn("#define RESOURCE_PAGE 65", source)
        self.assertIn("#define BILL_TARGET_BYTES 460", source)
        self.assertIn("bill_mutation_offset", source)
        self.assertIn("target_dsp", source)
        self.assertIn("response_interrupt_shadow = interrupt_shadow", source)
        self.assertIn("final_interrupt_shadow = interrupt_shadow", source)
        self.assertIn("non_target_indices_unchanged", source)
        self.assertIn("EXPECTED_SHA256", wrapper)
        self.assertIn("--post-official-bill-dsp", wrapper)
        self.assertIn("#define READBACK_COMMAND 0x000c0004", source)
        self.assertIn("#define READBACK_DWORDS 4", source)
        self.assertIn("#define READBACK_RESPONSE_WORDS (READBACK_DWORDS + 2)", source)
        self.assertIn("iommu_unmap_succeeded", source)
        self.assertIn("--post-official-bill-readback", wrapper)


class ProgramRuntimeABITests(unittest.TestCase):
    def test_runtime_abi_decoder_is_hash_locked_and_structural(self):
        module = load_tool("inspect_program_runtime_abi")
        self.assertEqual(
            module.KNOWN_SHA256,
            "7b664e8ad67b8104d9797defcc0c707fff55ae4981a725559ed9554f2f55cdf6",
        )
        meanings = " ".join(item[2] for item in module.SIGNATURES)
        self.assertIn("0x000c0004", meanings)
        self.assertIn("0x00150000", meanings)
        self.assertIn("low 24-bit offset", meanings)
        self.assertIn("first private resource", meanings)
        self.assertIn("four-dword main command", meanings)
        self.assertNotIn("authorization change", meanings.lower())
        source = (ROOT / "tools" / "inspect_program_runtime_abi.py").read_text()
        self.assertIn('"native_bytes": 0xAF8', source)


class Experiment032SourceTests(unittest.TestCase):
    def test_complete_resource_pass_is_exact_bounded_and_fail_closed(self):
        source = (ROOT / "tools" / "vfio_realverb_sequence.c").read_text()
        wrapper = (ROOT / "tools" / "uad2-vfio-realverb-sequence.sh").read_text()
        self.assertIn("#define RESOURCE_COUNT 13", source)
        self.assertIn("#define CHUNK_COUNT 16", source)
        self.assertIn("#define PROCESS_TICKS_MAX 8", source)
        self.assertIn(
            "#define PAGE_COUNT (PROCESS_OUTPUT_PAGE_BASE +", source
        )
        self.assertIn("#define RESOURCE_WAIT_MS 600", source)
        self.assertIn("#define READBACK_WAIT_MS 6000", source)
        self.assertIn("definition->total_bytes != definition->body_bytes + 28U", source)
        self.assertIn("resource_result->response[2] == definition->id", source)
        self.assertIn("resource_result->response[3] == definition->command", source)
        self.assertIn("memory_writes_bounded", source)
        self.assertIn("non_target_indices_unchanged", source)
        self.assertIn("VFIO_DEVICE_RESET", source)
        self.assertIn("iommu_unmap_succeeded", source)
        self.assertIn("#define ZERO_COMMAND_COUNT 33", source)
        self.assertIn("#define MEMSPEC_DWORDS 65", source)
        self.assertIn("0x00080004", source)
        self.assertIn("0x00150041", source)
        self.assertIn("cleanup_probe", source)
        self.assertIn("0x00030002", source)
        self.assertIn("unload_consumed_count == RESOURCE_COUNT", source)
        self.assertEqual(wrapper.count("verify_chunk boundary-"), 17)
        self.assertIn("experiment-029-deadline-safe-sequence", wrapper)
        self.assertIn("--allocation", wrapper)
        self.assertIn("UAD2_ALLOW_ONE_SHOT_RESOURCE_PASS", wrapper)
        self.assertIn(
            "YES_I_ACCEPT_OFFICIAL_REACTIVATION_MAY_BE_REQUIRED", wrapper
        )
        self.assertIn(
            "fe326c8a6a7d0b40e7958c14debe8ea1bc8d0fb160f7816bbaf9899b83590e84",
            wrapper,
        )
        self.assertIn(
            "0c353512fb27ed961b4e0746de7f1bbc462447f6e2c0263bc6209cda7b7718d0",
            wrapper,
        )
        self.assertIn("bus mastering was enabled before VFIO bind", wrapper)

    def test_process_coupled_readback_modes_are_bounded_and_fail_closed(self):
        source = (ROOT / "tools" / "vfio_realverb_sequence.c").read_text()
        wrapper = (ROOT / "tools" / "uad2-vfio-realverb-sequence.sh").read_text()
        self.assertIn("#define READBACK_MAX_DWORDS 430", source)
        self.assertIn("#define READBACK_PRIVATE_OFFSET 419", source)
        self.assertIn("PROCESS_FLAGS | (process_readback_probe ? 2U : 0U)", source)
        self.assertIn("readback_response_valid", source)
        self.assertIn('strcmp(argv[1], "--process-readback-dsp")', source)
        self.assertIn('strcmp(argv[1], "--process-snapshot-dsp")', source)
        self.assertIn('strcmp(argv[1], "--process-public-snapshot-dsp")', source)
        self.assertIn('strcmp(argv[1], "--process-private-snapshot-dsp")', source)
        self.assertIn("parsed_index >= ZERO_COMMAND_COUNT - 1", source)
        self.assertIn("zero_commands[parsed_index][3] > READBACK_MAX_DWORDS", source)
        self.assertIn('[ "$1" = "--process-readback-dsp" ]', wrapper)
        self.assertIn('[ "$1" = "--process-snapshot-dsp" ]', wrapper)
        self.assertIn('[ "$1" = "--process-public-snapshot-dsp" ]', wrapper)
        self.assertIn('[ "$1" = "--process-private-snapshot-dsp" ]', wrapper)
        self.assertIn("UAD2_ALLOW_BOUNDED_PRIVATE_READBACK", wrapper)


class Experiment018SourceTests(unittest.TestCase):
    def test_loader_probe_uses_bounded_pages_and_recovery(self):
        source = (ROOT / "tools" / "vfio_loader_rejection.c").read_text()
        self.assertIn("#define PAGE_COUNT 67", source)
        self.assertIn("#define LOADER_COMMAND_BASE 0x00120000", source)
        self.assertIn("#define LOADER_RESPONSE_CLASS 0x80040000", source)
        self.assertIn("payload_stat.st_size > (off_t)PAGE_SIZE_4K", source)
        self.assertIn("ioctl(device, VFIO_DEVICE_RESET)", source)
        self.assertNotIn("write32(bar, 0x8000", source)
        self.assertNotIn("write32(bar, 0xa000", source)

    def test_official_and_alternate_framings_are_explicit(self):
        source = (ROOT / "tools" / "vfio_loader_rejection.c").read_text()
        self.assertIn('strcmp(argv[1], "--single-buffer")', source)
        self.assertIn('"official-chained-send-block"', source)
        self.assertIn('"alternate-single-buffer-with-response-class"', source)


class ComputeDriverContractTests(unittest.TestCase):
    def test_uapi_exposes_capabilities_without_raw_mmio(self):
        header = (ROOT / "include" / "uapi" / "uad2_compute.h").read_text()
        self.assertIn("UAD2_COMPUTE_ABI_VERSION 2U", header)
        self.assertIn("UAD2_COMPUTE_FRAME_BYTES", header)
        self.assertIn("UAD2_CAP_RING_TRANSPORT", header)
        self.assertIn("UAD2_CAP_PROGRAM_LOAD", header)
        self.assertIn("UAD2_CAP_DMA_BUFFERS", header)
        self.assertIn("UAD2_CAP_JOB_COMPLETION", header)
        self.assertIn("UAD2_CAP_PROGRAM_ISOLATION", header)
        self.assertIn("UAD2_COMPUTE_IOC_GET_DSP_STATUS", header)
        self.assertIn("UAD2_COMPUTE_IOC_ALLOC_BUFFER", header)
        self.assertIn("UAD2_COMPUTE_IOC_LOAD_PROGRAM", header)
        self.assertIn("UAD2_COMPUTE_IOC_SUBMIT_JOB", header)
        self.assertIn("UAD2_COMPUTE_IOC_WAIT_JOB", header)
        self.assertNotIn("MMIO", header)
        self.assertNotIn("PHYSICAL", header)
        self.assertNotIn("SUBMIT_COMMAND", header)

    def test_driver_is_locked_to_exact_octo_and_bounded_pages(self):
        source = (ROOT / "kernel" / "uad2_compute.c").read_text()
        uapi = (ROOT / "include" / "uapi" / "uad2_compute.h").read_text()
        self.assertIn("#define UAD2_SUBDEVICE_OCTO 0x0005", source)
        self.assertIn("#define UAD2_BAR0_SIZE 0x10000", source)
        self.assertIn("#define UAD2_DSP_COUNT 8", source)
        self.assertIn("#define UAD2_RING_COUNT 2", source)
        self.assertIn("#define UAD2_RING_PAGES 4", source)
        self.assertIn("dma_alloc_coherent", source)
        self.assertIn("dma_mmap_coherent", source)
        self.assertIn(".mmap = uad2_mmap", source)
        self.assertIn("unmap_mapping_range", source)
        self.assertIn("__aligned_u64 capabilities", uapi)
        self.assertNotIn(".write =", source)
        self.assertNotIn(".read =", source)

    def test_driver_advertises_validated_version_2_operations(self):
        source = (ROOT / "kernel" / "uad2_compute.c").read_text()
        capability_assignment = source[source.index(".capabilities =") :]
        capability_assignment = capability_assignment[: capability_assignment.index(";")]
        self.assertIn("UAD2_CAP_RING_TRANSPORT", capability_assignment)
        self.assertIn("UAD2_CAP_PER_DSP_RESET", capability_assignment)
        self.assertIn("UAD2_CAP_PROGRAM_LOAD", capability_assignment)
        self.assertIn("UAD2_CAP_DMA_BUFFERS", capability_assignment)
        self.assertIn("UAD2_CAP_JOB_COMPLETION", capability_assignment)
        self.assertIn("UAD2_CAP_PROGRAM_ISOLATION", capability_assignment)

    def test_compute_operations_reach_bounded_ioctls(self):
        source = (ROOT / "lib" / "uad2_compute.c").read_text()
        client = (ROOT / "lib" / "uad2ctl.c").read_text()
        self.assertNotIn("return -EOPNOTSUPP;", source)
        self.assertIn("UAD2_COMPUTE_IOC_ALLOC_BUFFER", source)
        self.assertIn("UAD2_COMPUTE_IOC_LOAD_PROGRAM", source)
        self.assertIn("UAD2_COMPUTE_IOC_SUBMIT_JOB", source)
        self.assertIn("UAD2_COMPUTE_IOC_WAIT_JOB", source)
        self.assertIn("memcmp(output, input, UAD2_COMPUTE_FRAME_BYTES)", client)

    def test_program_loader_accepts_only_the_exact_authorized_bundle(self):
        source = (ROOT / "kernel" / "uad2_compute.c").read_text()
        packer = (ROOT / "tools" / "pack_realverb_program.py").read_text()
        self.assertIn("#define UAD2_PROGRAM_IMAGE_PAGES 17", source)
        self.assertIn("#define UAD2_PROGRAM_RESOURCES 13", source)
        self.assertIn("UAD2_BILL_MAGIC", source)
        self.assertIn("-EKEYREJECTED", source)
        self.assertIn("private_bundle=true do_not_commit=true", packer)
        self.assertEqual(packer.count('command-') , 17)

    def test_process_path_uses_private_resource_not_public_bill_allocation(self):
        driver = (ROOT / "kernel" / "uad2_compute.c").read_text()
        probe = (ROOT / "tools" / "vfio_realverb_sequence.c").read_text()
        wrapper = (ROOT / "tools" / "uad2-vfio-realverb-sequence.sh").read_text()
        self.assertIn("#define UAD2_PROCESS_ADDRESS 0x0009d00a", driver)
        self.assertIn("#define UAD2_PROCESS_INPUT_DWORDS 0x42", driver)
        self.assertIn("#define UAD2_PROCESS_OUTPUT_DWORDS 0x44", driver)
        self.assertIn("uad2_word_canary", driver)
        self.assertIn("#define PROCESS_PLUGIN_ADDRESS 0x0009d00a", probe)
        self.assertIn("#define PROCESS_RESPONSE_MARKER 0xf001000e", probe)
        self.assertIn('"--process-stream-dsp"', probe)
        self.assertIn('[ "$1" = "--process-stream-dsp" ]', wrapper)


class SharcImageInspectorTests(unittest.TestCase):
    @staticmethod
    def minimal_sharc_elf():
        names = b"\0.shstrtab\0"
        section_offset = 52
        names_offset = section_offset + 2 * 40
        ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
        header = struct.pack(
            "<16sHHIIIIIHHHHHH",
            ident,
            2,
            0x85,
            1,
            0,
            0,
            section_offset,
            0,
            52,
            0,
            0,
            40,
            2,
            1,
        )
        null_section = bytes(40)
        name_section = struct.pack(
            "<IIIIIIIIII", 1, 3, 0, 0, names_offset, len(names), 0, 0, 1, 0
        )
        return header + null_section + name_section + names

    def test_minimal_sharc_elf_is_recognized_and_contract_is_strict(self):
        module = load_tool("inspect_sharc_image")
        result = module.inspect_elf(self.minimal_sharc_elf())

        self.assertEqual(result["format"], "elf32-sharc")
        self.assertEqual(result["elf_type"], "executable")
        self.assertEqual(result["relocation_count"], 0)
        module.validate_contract(result, require_no_relocations=True)
        with self.assertRaisesRegex(module.ImageError, "required global function"):
            module.validate_contract(result, require_symbol="_uad_affine_entry")

    def test_non_sharc_machine_is_rejected(self):
        module = load_tool("inspect_sharc_image")
        image = bytearray(self.minimal_sharc_elf())
        struct.pack_into("<H", image, 18, 3)
        with self.assertRaisesRegex(module.ImageError, "not SHARC"):
            module.inspect_elf(bytes(image))

    def test_flat_v6_sharc_module_layout_is_decoded(self):
        module = load_tool("inspect_sharc_image")
        section_name = b".text\0"
        section_table = 56
        relocation_offset = section_table + 28
        name_offset = relocation_offset + 12
        content_offset = name_offset + len(section_name)
        header = (
            b"bFLT"
            + struct.pack(
                ">13I",
                6,
                2,
                section_table,
                1,
                relocation_offset,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            )
        )
        section = struct.pack(
            ">7I", name_offset, content_offset, 6, 4, 6, 1, 0
        )
        relocation = struct.pack(">III", 0, 0, 0x00000100)
        result = module.inspect_dlm(
            header + section + relocation + section_name + bytes(6)
        )

        self.assertEqual(result["format"], "adi-flat-v6-sharc")
        self.assertEqual(result["byte_order"], "big")
        self.assertEqual(result["sections"][0]["name"], ".text")
        self.assertTrue(result["sections"][0]["code"])
        self.assertEqual(result["relocation_count"], 1)
        self.assertEqual(
            result["relocation_counts_by_dynamic_type"], {"0x01": 1}
        )
        module.validate_contract(result, max_code_bytes=6)
        with self.assertRaisesRegex(module.ImageError, "export string and symbol table"):
            module.validate_contract(
                result, require_export_name="_uad_affine_entry"
            )

    def test_flat_v6_named_export_is_decoded_and_required(self):
        module = load_tool("inspect_sharc_image")
        section_table = 56
        section_count = 3
        names_offset = section_table + section_count * 28
        names = b".text\0.expstr\0.expsym\0"
        text_offset = names_offset + len(names)
        export_name = b"_uad_affine_entry\0"
        export_strings = b"".join(b"\0\0\0" + bytes([value]) for value in export_name)
        string_offset = text_offset + 6
        symbol_offset = string_offset + len(export_strings)
        header = (
            b"bFLT"
            + struct.pack(
                ">13I",
                6,
                2,
                section_table,
                section_count,
                names_offset,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            )
        )
        sections = b"".join(
            [
                struct.pack(
                    ">7I", names_offset, text_offset, 6, 4, 6, 1, 0
                ),
                struct.pack(
                    ">7I",
                    names_offset + len(b".text\0"),
                    string_offset,
                    len(export_strings),
                    1,
                    4,
                    0,
                    0,
                ),
                struct.pack(
                    ">7I",
                    names_offset + len(b".text\0.expstr\0"),
                    symbol_offset,
                    8,
                    1,
                    4,
                    0,
                    0,
                ),
            ]
        )
        result = module.inspect_dlm(
            header
            + sections
            + names
            + bytes(6)
            + export_strings
            + bytes(8)
        )

        self.assertEqual(result["exported_names"], ["_uad_affine_entry"])
        module.validate_contract(
            result, require_export_name="_uad_affine_entry"
        )
        with self.assertRaisesRegex(module.ImageError, "required DLM export"):
            module.validate_contract(result, require_export_name="_not_present")

    def test_standard_loader_stream_is_parsed_sequentially(self):
        module = load_tool("inspect_sharc_image")
        stream = (
            struct.pack(">III", 2, 5, 0x8C000)
            + struct.pack(">III", 5, 2, 0x8C005)
            + bytes(12)
            + struct.pack(">III", 0, 0, 0)
        )
        result = module.inspect_ldr(stream)

        self.assertEqual(result["byte_order"], "big")
        self.assertEqual(
            [block["tag"] for block in result["blocks"]],
            ["ZERO_L48", "INIT_L48", "FINAL_INIT"],
        )
        self.assertEqual(result["blocks"][1]["payload_bytes"], 12)


class SharcMemoryAliasTests(unittest.TestCase):
    def test_realverb_private_resources_one_and_two_are_exact_pm48_spans(self):
        module = load_tool("sharc_memory_alias")
        upper = module.dm32_span_to_pm48(0x9CF74, 0x96)
        lower = module.dm32_span_to_pm48(0x9CEDE, 0x96)

        self.assertTrue(upper["whole_pm48_span"])
        self.assertTrue(lower["whole_pm48_span"])
        self.assertEqual(upper["pm48_start"], "0x000934f8")
        self.assertEqual(lower["pm48_start"], "0x00093494")
        self.assertEqual(upper["pm48_words"], 100)
        self.assertEqual(lower["pm48_words"], 100)
        self.assertEqual(lower["pm48_end_inclusive"], "0x000934f7")

    def test_unaligned_data_span_is_not_mislabeled_as_code(self):
        module = load_tool("sharc_memory_alias")
        result = module.dm32_span_to_pm48(0x9CEA6, 0x38)
        self.assertFalse(result["whole_pm48_span"])
        self.assertNotIn("pm48_words", result)


class AffineReferenceTests(unittest.TestCase):
    def test_separate_binary32_rounding_and_bits_are_stable(self):
        module = load_tool("affine_reference")
        result = module.generate([1.0, -1.0, 1.0 / 3.0], 0.625, -0.09375)

        self.assertEqual(result["scale"]["bits"], "0x3f200000")
        self.assertEqual(result["bias"]["bits"], "0xbdc00000")
        self.assertEqual(
            [item["bits"] for item in result["expected_output"]],
            ["0x3f080000", "0xbf380000", "0x3deaaaac"],
        )

    def test_nonfinite_and_oversized_vectors_are_rejected(self):
        module = load_tool("affine_reference")
        with self.assertRaisesRegex(ValueError, "finite"):
            module.generate([float("nan")], 1.0, 0.0)
        with self.assertRaisesRegex(ValueError, "1..64"):
            module.generate([0.0] * 65, 1.0, 0.0)

    def test_committed_canonical_vector_matches_the_oracle(self):
        module = load_tool("affine_reference")
        committed = __import__("json").loads(
            (ROOT / "kernels" / "affine" / "canonical-vector.json").read_text()
        )
        generated = module.generate(
            list(module.CANONICAL_INPUT),
            module.CANONICAL_SCALE,
            module.CANONICAL_BIAS,
        )

        self.assertEqual(
            committed["input_bits"],
            [item["bits"] for item in generated["input"]],
        )
        self.assertEqual(
            committed["expected_output_bits"],
            [item["bits"] for item in generated["expected_output"]],
        )


if __name__ == "__main__":
    unittest.main()
