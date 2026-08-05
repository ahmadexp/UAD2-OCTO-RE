import importlib.util
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

    def test_unknown_loader_driver_is_refused(self):
        module = load_tool("inspect_pcie_loader")
        with tempfile.NamedTemporaryFile() as candidate:
            candidate.write(b"not the PCIe driver")
            candidate.flush()
            with self.assertRaisesRegex(ValueError, "driver hash mismatch"):
                module.inspect(pathlib.Path(candidate.name))


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
        self.assertEqual(result["schema"], 2)
        self.assertEqual(result["entropy_bits_per_byte"]["inner_core"]["count"], 1)
        self.assertEqual(
            result["standard_digest_subsequence_hypotheses"]["sha256_inner_core"],
            {"eligible": 1, "matches_anywhere_in_prefix": 0},
        )
        self.assertEqual(
            result["aligned_16_byte_block_tests"]["distinct_blocks_shared_by_multiple_unique_resources"],
            0,
        )


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
        self.assertIn("#define PAGE_COUNT 65", source)
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
        self.assertIn("UAD2_CAP_RING_TRANSPORT", header)
        self.assertIn("UAD2_CAP_PROGRAM_ISOLATION", header)
        self.assertIn("UAD2_COMPUTE_IOC_GET_DSP_STATUS", header)
        self.assertNotIn("MMIO", header)
        self.assertNotIn("PHYSICAL", header)
        self.assertNotIn("SUBMIT_COMMAND", header)

    def test_driver_is_locked_to_exact_octo_and_bounded_pages(self):
        source = (ROOT / "kernel" / "uad2_compute.c").read_text()
        self.assertIn("#define UAD2_SUBDEVICE_OCTO 0x0005", source)
        self.assertIn("#define UAD2_BAR0_SIZE 0x10000", source)
        self.assertIn("#define UAD2_DSP_COUNT 8", source)
        self.assertIn("#define UAD2_RING_COUNT 2", source)
        self.assertIn("#define UAD2_RING_PAGES 4", source)
        self.assertIn("dma_alloc_coherent", source)
        self.assertNotIn(".mmap", source)
        self.assertNotIn(".write =", source)
        self.assertNotIn(".read =", source)

    def test_driver_advertises_only_validated_operations(self):
        source = (ROOT / "kernel" / "uad2_compute.c").read_text()
        capability_assignment = source[source.index(".capabilities =") :]
        capability_assignment = capability_assignment[: capability_assignment.index(";")]
        self.assertIn("UAD2_CAP_RING_TRANSPORT", capability_assignment)
        self.assertIn("UAD2_CAP_PER_DSP_RESET", capability_assignment)
        self.assertNotIn("UAD2_CAP_PROGRAM_LOAD", capability_assignment)
        self.assertNotIn("UAD2_CAP_DMA_BUFFERS", capability_assignment)
        self.assertNotIn("UAD2_CAP_JOB_COMPLETION", capability_assignment)

    def test_compute_operations_fail_closed_in_userspace(self):
        source = (ROOT / "lib" / "uad2_compute.c").read_text()
        self.assertEqual(source.count("return -EOPNOTSUPP;"), 4)


if __name__ == "__main__":
    unittest.main()
