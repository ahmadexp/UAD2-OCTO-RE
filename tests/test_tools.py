import importlib.util
import pathlib
import struct
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


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


class BillContainerInspectorTests(unittest.TestCase):
    def test_parser_reproduces_official_tail_transform(self):
        module = load_tool("inspect_bill_container")
        resource_id = 0x020000C2
        body = bytes(range(32)) + bytes(12)
        data = struct.pack(
            "<4s4I", b"Bill", resource_id, 0x02000000, len(body), 3
        ) + body
        result = module.parse(data)
        expected_tail = module.replacement_stream(resource_id, 3)

        self.assertEqual(result["resource_id"], "0x020000c2")
        self.assertEqual(result["dsp_generation"], 2)
        self.assertEqual(result["preserved_bytes"], len(data) - 12)
        self.assertEqual(result["transformed"][-12:], expected_tail)
        self.assertFalse(result["input_tail_already_transformed"])

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


if __name__ == "__main__":
    unittest.main()
