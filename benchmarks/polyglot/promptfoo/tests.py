"""Generate the fixed mini-v2 test matrix for Promptfoo."""

from __future__ import annotations

import importlib.util
from pathlib import Path


def _load_harness():
    path = Path(__file__).resolve().parents[1] / "scripts" / "run_eval.py"
    spec = importlib.util.spec_from_file_location("aider_polyglot_harness_tests", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load benchmark harness: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def generate_tests(config=None):
    config = config or {}
    harness = _load_harness()
    corpus_value = config.get("corpus")
    if not corpus_value:
        raise RuntimeError("tests config requires corpus")
    corpus = Path(corpus_value).expanduser().resolve()
    commit = harness.validate_corpus(corpus, False)
    cases = []
    for task_id in harness.DEFAULT_TASKS:
        language, source = harness.task_source(corpus, task_id)
        cases.append(
            {
                "description": task_id,
                "vars": {
                    "task": task_id,
                    "language": language,
                    "prompt": harness.public_prompt(source),
                },
                "metadata": {
                    "suite": harness.SUITE,
                    "corpus_commit": commit,
                    "task": task_id,
                    "language": language,
                },
            }
        )
    return cases
