export LC_ALL=C GIT_OPTIONAL_LOCKS=0
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_EXTERNAL_DIFF
g() { git --no-optional-locks --literal-pathspecs -c core.fsmonitor=false -c core.quotePath=true "$@"; }
repo=$(g rev-parse --is-inside-work-tree 2>&1) || {
  case "$repo" in *'not a git repository'*) printf 'none\n'; exit 0;; esac
  exit 1
}
[ "$repo" = true ] || { printf 'none\n'; exit 0; }
base=$(g rev-parse --verify --quiet HEAD) || base=$(g hash-object -t tree --stdin) || exit 1
printf 'repo\n'
g status --porcelain=v1 --untracked-files=all --ignore-submodules=none -- . || exit 1
printf 'stats\n'
g diff --numstat --no-ext-diff --no-textconv --ignore-submodules=none "$base" -- . || exit 1
printf 'done\n'
