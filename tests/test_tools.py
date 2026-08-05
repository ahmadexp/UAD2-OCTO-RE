import importlib.util
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def load_tool(name: str):
    path = ROOT / "tools" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
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
        self.assertEqual(len(addresses), 36)
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


if __name__ == "__main__":
    unittest.main()
