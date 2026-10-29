import copy
import json
import unittest
from pathlib import Path

from tools.validate_samsung_device_profile import ProfileError, validate_profile


PROFILE_PATH = Path(__file__).parents[1] / "src/targets/samsung-g986u1-4.19.113/device-profile.json"


class SamsungDeviceProfileValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profile = json.loads(PROFILE_PATH.read_text(encoding="utf-8"))

    def test_repository_profile_passes_identity_schema(self):
        messages = validate_profile(self.profile)
        self.assertTrue(any("evidence-only" in message for message in messages))

    def test_rejects_wrong_device_identity(self):
        profile = copy.deepcopy(self.profile)
        profile["device"]["model"] = "SM-G986U"
        with self.assertRaisesRegex(ProfileError, "device.model mismatch"):
            validate_profile(profile)

    def test_rejects_executable_status(self):
        profile = copy.deepcopy(self.profile)
        profile["status"] = "ready"
        with self.assertRaisesRegex(ProfileError, "identity-check-only"):
            validate_profile(profile)

    def test_rejects_malformed_artifact_digest(self):
        profile = copy.deepcopy(self.profile)
        profile["artifacts"]["vendor_elf"]["sha256"] = "not-a-digest"
        with self.assertRaisesRegex(ProfileError, "lowercase SHA-256"):
            validate_profile(profile)

    def test_rejects_missing_required_config(self):
        profile = copy.deepcopy(self.profile)
        del profile["kernel_config"]["required"]["CONFIG_FUTEX_PI"]
        with self.assertRaisesRegex(ProfileError, "CONFIG_FUTEX_PI is missing"):
            validate_profile(profile)

    def test_rejects_wrong_boot_layout(self):
        profile = copy.deepcopy(self.profile)
        profile["boot_image"]["header_version"] = 3
        with self.assertRaisesRegex(ProfileError, "boot_image.header_version mismatch"):
            validate_profile(profile)


if __name__ == "__main__":
    unittest.main()
