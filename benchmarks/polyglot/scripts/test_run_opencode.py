import importlib.util
import json
from pathlib import Path
import unittest


SCRIPT = Path(__file__).with_name("run_opencode.py")
SPEC = importlib.util.spec_from_file_location("run_opencode", SCRIPT)
assert SPEC and SPEC.loader
RUN_OPENCODE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN_OPENCODE)


class ReasoningOverrideTests(unittest.TestCase):
    def test_pin_and_sort_preserve_reasoning(self):
        config = json.loads(RUN_OPENCODE.reasoning_override(
            "openrouter/deepseek/deepseek-v4-pro", "medium", "throughput", "fixture"))
        model = config["provider"]["openrouter"]["models"]["deepseek/deepseek-v4-pro"]
        self.assertEqual(model["options"]["provider"], {
            "sort": "throughput", "only": ["fixture"], "allow_fallbacks": False})
        self.assertEqual(model["variants"]["medium"]["reasoning"], {"effort": "medium"})

    def test_routing_without_reasoning_override(self):
        config = json.loads(RUN_OPENCODE.reasoning_override(
            "openrouter/deepseek/deepseek-v4-pro", None, "throughput"))
        model = config["provider"]["openrouter"]["models"]["deepseek/deepseek-v4-pro"]
        self.assertNotIn("variants", model)
        self.assertEqual(model["options"], {"provider": {"sort": "throughput"}})

    def test_openrouter_medium_maps_to_openrouter_reasoning_body(self):
        config = json.loads(
            RUN_OPENCODE.reasoning_override("openrouter/deepseek/deepseek-v4-pro", "medium")
        )
        self.assertEqual(
            config["provider"]["openrouter"]["models"]["deepseek/deepseek-v4-pro"]["variants"],
            {"medium": {"reasoning": {"effort": "medium"}}},
        )

    def test_reasoning_override_rejects_non_openrouter_model(self):
        with self.assertRaisesRegex(ValueError, "OpenRouter model"):
            RUN_OPENCODE.reasoning_override("deepseek/deepseek-v4-pro", "medium")


if __name__ == "__main__":
    unittest.main()
