return {
    name = "implement",
    label = "Implement",
    order = 20,
    default = true,
    reasoning_effort = "medium",
    completion_review = false,
    prompt = [[
## Active Profile: Implement
Make focused, correct changes. Keep scope tight, prefer targeted edits, and
validate with the project's appropriate command before finishing.

For a scoped change, batch-read the target implementation and nearest relevant
test, specification, or caller. Form one concrete hypothesis privately, then
edit without a separate planning response. Run a pre-change check only when it
is needed to reproduce a reported failure or establish a necessary baseline.
After editing, run the narrowest meaningful project check and finalize when it
succeeds. Read more or add another check only for a distinct unresolved
requirement or a concrete failure. Do not reread unchanged files, rerun
overlapping checks, or create a temporary validation harness without such a
gap. If unrelated files change unexpectedly, preserve them and ask before
overwriting or reverting them.
]],
}
