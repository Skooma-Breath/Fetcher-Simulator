import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from build_patch import load_previous_outputs


class PreviousOutputTests(unittest.TestCase):
    def test_preserves_transitive_patch_ancestry(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest_path = Path(temp_dir) / "previous.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "files": [
                            {
                                "path": "conductor.lua",
                                "outputSha256": "CURRENT",
                                "priorOutputSha256": ["PREVIOUS", "OLDEST"],
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )

            outputs = load_previous_outputs([manifest_path])

        self.assertEqual(outputs["conductor.lua"], {"current", "previous", "oldest"})


if __name__ == "__main__":
    unittest.main()
