#!/usr/bin/env bash
set -u

SCRIPT_NAME="$(basename "$0")"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

RUNS=5
WORKDIR="$(pwd)"
OUT_ROOT="$REPO_ROOT/benchmarks/results"
TASKS_DIR=""
RESET_CMD=""
ISOLATED_HOME=0
IDLE_SECONDS=0
ORDER_MODE="alternating"

AGENT_A_NAME="capstan"
AGENT_A_BIN="capstan"
AGENT_A_CMD="{bin} run --benchmark --prompt-file {prompt_file} --workdir {workdir}"
AGENT_A_SELF_CMD=""
AGENT_A_IDLE_CMD=""

AGENT_B_NAME="opencode"
AGENT_B_BIN="opencode"
AGENT_B_CMD="{bin} run --dir {workdir} {prompt_text}"
AGENT_B_SELF_CMD=""
AGENT_B_IDLE_CMD=""

KEEP_GOING=1

usage() {
  cat <<'EOF'
Usage:
  agent-bench.sh [options]

Compares two CLI agents by binary size, help/startup cost, and repeated
task execution time. Results are written to a timestamped directory.

Common:
  --runs N                  Runs per task, default: 5
  --workdir DIR             Repository/workspace used by task commands
  --tasks-dir DIR           Directory with *.md prompts; default tasks are generated
  --out DIR                 Results root, default: benchmarks/results
  --reset-cmd CMD           Optional command run before each task run
  --isolated-home           Use a fresh HOME per run. Only use if API keys come from env
  --idle-seconds N          Enable idle sampling for agents with --agent-*-idle-cmd
  --order MODE              Run order: alternating, agent-block, or a-first
  --fail-fast               Stop after first failed task run

Agent A:
  --agent-a-name NAME
  --agent-a-bin PATH_OR_CMD
  --agent-a-cmd TEMPLATE
  --agent-a-self-cmd TEMPLATE  Optional project-specific offline smoke command
  --agent-a-idle-cmd TEMPLATE

Agent B:
  --agent-b-name NAME
  --agent-b-bin PATH_OR_CMD
  --agent-b-cmd TEMPLATE
  --agent-b-self-cmd TEMPLATE  Optional project-specific offline smoke command
  --agent-b-idle-cmd TEMPLATE

Short aliases:
  --capstan PATH            Set Agent A to Capstan at PATH
  --opencode PATH           Set Agent B to opencode at PATH

Templates:
  {bin}         shell-quoted agent binary/command
  {prompt_file} shell-quoted prompt file
  {prompt_text} shell-quoted prompt file contents
  {workdir}     shell-quoted workdir
  {out_dir}     shell-quoted current result directory
  {repo_root}   shell-quoted Capstan repository root

Example:
  benchmarks/footprint/agent-bench.sh \
    --capstan ./build/capstan \
    --agent-a-cmd '{bin} run --benchmark --provider openrouter --model deepseek/deepseek-v4-pro --prompt-file {prompt_file} --workdir {workdir}' \
    --opencode opencode \
    --agent-b-cmd 'python3 {repo_root}/benchmarks/footprint/opencode_prompt_file.py --bin {bin} --model openrouter/deepseek/deepseek-v4-pro --workdir {workdir} --prompt-file {prompt_file} --dangerously-skip-permissions' \
    --runs 5 \
    --workdir /path/to/disposable-workspace

Notes:
  - Use the same provider/model/API env for both agents, otherwise you benchmark
    the model/provider more than the agent runtime.
  - Context-window RAM is not measured generically because agents do not expose
    one shared history-import API. Add synthetic prompts in --tasks-dir for that.
EOF
}

die() {
  printf '%s: %s\n' "$SCRIPT_NAME" "$*" >&2
  exit 1
}

need_value() {
  if [ "$#" -lt 2 ]; then
    die "$1 requires a value"
  fi
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --help|-h) usage; exit 0 ;;
    --runs) need_value "$@"; RUNS="$2"; shift 2 ;;
    --workdir) need_value "$@"; WORKDIR="$2"; shift 2 ;;
    --tasks-dir) need_value "$@"; TASKS_DIR="$2"; shift 2 ;;
    --out) need_value "$@"; OUT_ROOT="$2"; shift 2 ;;
    --reset-cmd) need_value "$@"; RESET_CMD="$2"; shift 2 ;;
    --isolated-home) ISOLATED_HOME=1; shift ;;
    --idle-seconds) need_value "$@"; IDLE_SECONDS="$2"; shift 2 ;;
    --order) need_value "$@"; ORDER_MODE="$2"; shift 2 ;;
    --fail-fast) KEEP_GOING=0; shift ;;

    --agent-a-name) need_value "$@"; AGENT_A_NAME="$2"; shift 2 ;;
    --agent-a-bin) need_value "$@"; AGENT_A_BIN="$2"; shift 2 ;;
    --agent-a-cmd) need_value "$@"; AGENT_A_CMD="$2"; shift 2 ;;
    --agent-a-self-cmd) need_value "$@"; AGENT_A_SELF_CMD="$2"; shift 2 ;;
    --agent-a-idle-cmd) need_value "$@"; AGENT_A_IDLE_CMD="$2"; shift 2 ;;

    --agent-b-name) need_value "$@"; AGENT_B_NAME="$2"; shift 2 ;;
    --agent-b-bin) need_value "$@"; AGENT_B_BIN="$2"; shift 2 ;;
    --agent-b-cmd) need_value "$@"; AGENT_B_CMD="$2"; shift 2 ;;
    --agent-b-self-cmd) need_value "$@"; AGENT_B_SELF_CMD="$2"; shift 2 ;;
    --agent-b-idle-cmd) need_value "$@"; AGENT_B_IDLE_CMD="$2"; shift 2 ;;

    --capstan)
      need_value "$@"
      AGENT_A_NAME="capstan"
      AGENT_A_BIN="$2"
      AGENT_A_CMD="{bin} run --benchmark --prompt-file {prompt_file} --workdir {workdir}"
      AGENT_A_SELF_CMD=""
      shift 2
      ;;
    --opencode)
      need_value "$@"
      AGENT_B_NAME="opencode"
      AGENT_B_BIN="$2"
      shift 2
      ;;
    *) die "unknown option: $1" ;;
  esac
done

case "$RUNS" in
  ''|*[!0-9]*) die "--runs must be a positive integer" ;;
esac
[ "$RUNS" -gt 0 ] || die "--runs must be > 0"

case "$IDLE_SECONDS" in
  ''|*[!0-9]*) die "--idle-seconds must be a non-negative integer" ;;
esac

case "$ORDER_MODE" in
  alternating|agent-block|a-first) ;;
  *) die "--order must be one of: alternating, agent-block, a-first" ;;
esac

if ! command -v python3 >/dev/null 2>&1; then
  die "python3 is required for statistics and portable timestamps"
fi

if [ ! -d "$WORKDIR" ]; then
  die "workdir does not exist: $WORKDIR"
fi

timestamp="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="$OUT_ROOT/$timestamp"
RAW_DIR="$RUN_DIR/raw"
mkdir -p "$RAW_DIR" || die "cannot create result directory: $RUN_DIR"

log() {
  printf '%s\n' "$*"
}

shq() {
  local s="${1-}"
  printf "'%s'" "${s//\'/\'\\\'\'}"
}

render_cmd() {
  local template="$1"
  local bin="$2"
  local prompt_file="${3-}"
  local out_dir="${4-}"
  local prompt_text=""
  local rendered="$template"
  if [ -n "$prompt_file" ] && [ -f "$prompt_file" ]; then
    prompt_text="$(cat "$prompt_file")"
  fi
  rendered="${rendered//\{bin\}/$(shq "$bin")}"
  rendered="${rendered//\{prompt_file\}/$(shq "$prompt_file")}"
  rendered="${rendered//\{prompt_text\}/$(shq "$prompt_text")}"
  rendered="${rendered//\{workdir\}/$(shq "$WORKDIR")}"
  rendered="${rendered//\{out_dir\}/$(shq "$out_dir")}"
  rendered="${rendered//\{repo_root\}/$(shq "$REPO_ROOT")}"
  printf '%s' "$rendered"
}

resolve_bin() {
  local bin="$1"
  if [[ "$bin" == */* ]]; then
    if [ -e "$bin" ]; then
      printf '%s/%s' "$(cd "$(dirname "$bin")" && pwd)" "$(basename "$bin")"
    else
      printf '%s' "$bin"
    fi
  else
    command -v "$bin" 2>/dev/null || printf '%s' "$bin"
  fi
}

human_size() {
  local path="$1"
  if [ -f "$path" ]; then
    ls -lh "$path" | awk '{print $5}'
  else
    printf ''
  fi
}

byte_size() {
  local path="$1"
  if [ -f "$path" ]; then
    wc -c < "$path" | tr -d ' '
  else
    printf ''
  fi
}

time_flag() {
  if /usr/bin/time -v true >/dev/null 2>"$RUN_DIR/.time-probe" 2>/dev/null; then
    printf -- '-v'
  elif /usr/bin/time -l true >/dev/null 2>"$RUN_DIR/.time-probe" 2>/dev/null; then
    printf -- '-l'
  else
    printf -- '-p'
  fi
}

TIME_FLAG="$(time_flag)"

create_default_tasks() {
  local dir="$1"
  mkdir -p "$dir" || die "cannot create tasks dir: $dir"

  cat > "$dir/01-bug-find.md" <<'EOF'
Find the bug in this C function. Explain the failure mode and provide the
smallest correct patch.

```c
int input_insert(char *buf, int cap, int *cursor, const char *text) {
  int n = strlen(buf);
  int add = strlen(text);
  memmove(buf + *cursor + add, buf + *cursor, n - *cursor + 1);
  memcpy(buf + *cursor, text, add);
  *cursor += add;
  return 0;
}
```
EOF

  cat > "$dir/02-async-rewrite.md" <<'EOF'
Rewrite this JavaScript function to async/await while preserving behavior,
including error handling and return shape.

```js
function loadUserBundle(api, userId, cb) {
  api.getUser(userId, function (err, user) {
    if (err) return cb(err);
    api.getTeams(user.id, function (err, teams) {
      if (err) return cb(err);
      api.getPermissions(user.id, function (err, permissions) {
        if (err) return cb(err);
        cb(null, { user: user, teams: teams, permissions: permissions });
      });
    });
  });
}
```
EOF

  cat > "$dir/03-commit-message.md" <<'EOF'
Write a concise conventional commit message for this diff.

```diff
diff --git a/src/input.c b/src/input.c
@@
-void input_insert(const char *s) {
+void input_insert(const char *s) {
   while (*s) {
+    if (g_cursor >= INPUT_BUFFER_SIZE - 1)
+      break;
     input_buffer[g_cursor++] = *s++;
   }
   input_buffer[g_cursor] = '\0';
 }
```
EOF

  cat > "$dir/04-repo-review-small.md" <<'EOF'
Inspect this repository at a high level. Identify the three most likely runtime
risks in a terminal LLM agent written in C, and name the files you would inspect
first. Do not modify files.
EOF
}

if [ -z "$TASKS_DIR" ]; then
  TASKS_DIR="$RUN_DIR/tasks"
  create_default_tasks "$TASKS_DIR"
else
  [ -d "$TASKS_DIR" ] || die "tasks dir does not exist: $TASKS_DIR"
fi

find "$TASKS_DIR" -maxdepth 1 -type f -name '*.md' | sort > "$RUN_DIR/task-list.txt"
if [ ! -s "$RUN_DIR/task-list.txt" ]; then
  die "no *.md tasks found in $TASKS_DIR"
fi

cat > "$RUN_DIR/config.txt" <<EOF
timestamp=$timestamp
runs=$RUNS
workdir=$WORKDIR
tasks_dir=$TASKS_DIR
time_flag=$TIME_FLAG
agent_a_name=$AGENT_A_NAME
agent_a_bin=$AGENT_A_BIN
agent_a_cmd=$AGENT_A_CMD
agent_a_self_cmd=$AGENT_A_SELF_CMD
agent_b_name=$AGENT_B_NAME
agent_b_bin=$AGENT_B_BIN
agent_b_cmd=$AGENT_B_CMD
agent_b_self_cmd=$AGENT_B_SELF_CMD
isolated_home=$ISOLATED_HOME
idle_seconds=$IDLE_SECONDS
reset_cmd=$RESET_CMD
order_mode=$ORDER_MODE
EOF

STATIC_CSV="$RUN_DIR/static.csv"
RUNS_CSV="$RUN_DIR/runs.csv"
IDLE_CSV="$RUN_DIR/idle.csv"

printf 'agent,bin,resolved_bin,exists,size_bytes,size_human,help_exit,help_elapsed_sec,help_log,self_exit,self_elapsed_sec,self_log\n' > "$STATIC_CSV"
printf 'agent,task,run,order_index,exit_code,elapsed_sec,user_seconds,system_seconds,cpu_seconds,max_rss_bytes,time_log,stdout_log,stderr_log\n' > "$RUNS_CSV"
printf 'agent,sample,cpu_percent,rss_kb\n' > "$IDLE_CSV"

run_timed_command() {
  local cmd="$1"
  local stdout_file="$2"
  local stderr_file="$3"
  local time_file="$4"

  local start end rc metrics
  start="$(python3 -c 'import time; print("%.9f" % time.time())')"
  set +e
  /usr/bin/time "$TIME_FLAG" bash -lc "$cmd" </dev/null >"$stdout_file" 2>"$stderr_file"
  rc=$?
  set -e
  end="$(python3 -c 'import time; print("%.9f" % time.time())')"
  cp "$stderr_file" "$time_file"
  elapsed="$(python3 -c 'import sys; print("%.6f" % (float(sys.argv[2]) - float(sys.argv[1])))' "$start" "$end")"
  metrics="$(python3 "$SCRIPT_DIR/parse_time.py" "$time_file")"
  printf '%s|%s|%s\n' "$rc" "$elapsed" "$metrics"
}

write_static_for_agent() {
  local name="$1"
  local bin="$2"
  local self_template="$3"
  local resolved exists size_bytes size_human help_exit help_elapsed help_log self_exit self_elapsed self_log cmd result result_rest stdout_file stderr_file time_file

  resolved="$(resolve_bin "$bin")"
  exists=0
  [ -f "$resolved" ] && exists=1
  size_bytes="$(byte_size "$resolved")"
  size_human="$(human_size "$resolved")"
  self_exit=""
  self_elapsed=""
  self_log=""

  help_log="$RAW_DIR/${name}.help.stderr"
  stdout_file="$RAW_DIR/${name}.help.stdout"
  stderr_file="$help_log"
  time_file="$RAW_DIR/${name}.help.time"
  cmd="$(render_cmd "{bin} --help" "$resolved" "" "$RUN_DIR")"
  result="$(run_timed_command "$cmd" "$stdout_file" "$stderr_file" "$time_file")"
  help_exit="${result%%|*}"
  result_rest="${result#*|}"
  help_elapsed="${result_rest%%|*}"

  if [ -n "$self_template" ]; then
    self_log="$RAW_DIR/${name}.self-test.stderr"
    stdout_file="$RAW_DIR/${name}.self-test.stdout"
    stderr_file="$self_log"
    time_file="$RAW_DIR/${name}.self-test.time"
    cmd="$(render_cmd "$self_template" "$resolved" "" "$RUN_DIR")"
    result="$(run_timed_command "$cmd" "$stdout_file" "$stderr_file" "$time_file")"
    self_exit="${result%%|*}"
    result_rest="${result#*|}"
    self_elapsed="${result_rest%%|*}"
  fi

  python3 - "$STATIC_CSV" "$name" "$bin" "$resolved" "$exists" "$size_bytes" "$size_human" "$help_exit" "$help_elapsed" "$help_log" "$self_exit" "$self_elapsed" "$self_log" <<'PY'
import csv, sys
path = sys.argv[1]
row = sys.argv[2:]
with open(path, "a", newline="") as f:
    csv.writer(f).writerow(row)
PY
}

run_reset() {
  local run_home="$1"
  if [ -n "$RESET_CMD" ]; then
    HOME="$run_home" bash -lc "$RESET_CMD"
  fi
}

run_task_for_agent() {
  local name="$1"
  local bin="$2"
  local template="$3"
  local task_file="$4"
  local run_num="$5"
  local order_index="$6"
  local resolved task_base run_base stdout_file stderr_file time_file cmd result rc elapsed user_seconds system_seconds cpu_seconds max_rss_bytes run_home

  resolved="$(resolve_bin "$bin")"
  task_base="$(basename "$task_file" .md)"
  run_base="${name}.${task_base}.run${run_num}"
  stdout_file="$RAW_DIR/${run_base}.stdout"
  stderr_file="$RAW_DIR/${run_base}.stderr"
  time_file="$RAW_DIR/${run_base}.time"
  run_home="$HOME"

  if [ "$ISOLATED_HOME" -eq 1 ]; then
    run_home="$RUN_DIR/home/${run_base}"
    mkdir -p "$run_home"
  fi

  run_reset "$run_home"
  cmd="$(render_cmd "$template" "$resolved" "$task_file" "$RUN_DIR")"
  result="$(HOME="$run_home" run_timed_command "$cmd" "$stdout_file" "$stderr_file" "$time_file")"
  IFS='|' read -r rc elapsed user_seconds system_seconds cpu_seconds max_rss_bytes <<EOF
$result
EOF

  python3 - "$RUNS_CSV" "$name" "$task_base" "$run_num" "$order_index" "$rc" "$elapsed" "$user_seconds" "$system_seconds" "$cpu_seconds" "$max_rss_bytes" "$time_file" "$stdout_file" "$stderr_file" <<'PY'
import csv, sys
path = sys.argv[1]
row = sys.argv[2:]
with open(path, "a", newline="") as f:
    csv.writer(f).writerow(row)
PY

  if [ "$rc" -ne 0 ] && [ "$KEEP_GOING" -eq 0 ]; then
    die "$name $task_base run $run_num failed; see $stderr_file"
  fi
}

run_order_for_index() {
  local run="$1"
  if [ "$ORDER_MODE" = "a-first" ]; then
    printf '%s\n%s\n' "a" "b"
  elif [ "$ORDER_MODE" = "alternating" ] && [ $((run % 2)) -eq 0 ]; then
    printf '%s\n%s\n' "b" "a"
  else
    printf '%s\n%s\n' "a" "b"
  fi
}

sample_idle_for_agent() {
  local name="$1"
  local bin="$2"
  local template="$3"
  local resolved cmd pid i line cpu rss

  [ "$IDLE_SECONDS" -gt 0 ] || return 0
  [ -n "$template" ] || return 0

  resolved="$(resolve_bin "$bin")"
  cmd="$(render_cmd "$template" "$resolved" "" "$RUN_DIR")"
  bash -lc "$cmd" >/dev/null 2>"$RAW_DIR/${name}.idle.stderr" &
  pid=$!
  sleep 1
  i=1
  while [ "$i" -le "$IDLE_SECONDS" ]; do
    if ! kill -0 "$pid" 2>/dev/null; then
      break
    fi
    line="$(ps -o %cpu= -o rss= -p "$pid" 2>/dev/null | awk 'NF >= 2 {print $1 "," $2}')"
    if [ -n "$line" ]; then
      cpu="${line%,*}"
      rss="${line#*,}"
      printf '%s,%s,%s,%s\n' "$name" "$i" "$cpu" "$rss" >> "$IDLE_CSV"
    fi
    sleep 1
    i=$((i + 1))
  done
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

log "Writing results to $RUN_DIR"
log "Timing mode: /usr/bin/time $TIME_FLAG"

write_static_for_agent "$AGENT_A_NAME" "$AGENT_A_BIN" "$AGENT_A_SELF_CMD"
write_static_for_agent "$AGENT_B_NAME" "$AGENT_B_BIN" "$AGENT_B_SELF_CMD"

if [ "$ORDER_MODE" = "agent-block" ]; then
  while IFS= read -r task_file; do
    task_base="$(basename "$task_file" .md)"
    log "Task: $task_base"
    order_index=1
    run=1
    while [ "$run" -le "$RUNS" ]; do
      log "  $AGENT_A_NAME run $run/$RUNS"
      run_task_for_agent "$AGENT_A_NAME" "$AGENT_A_BIN" "$AGENT_A_CMD" "$task_file" "$run" "$order_index"
      order_index=$((order_index + 1))
      run=$((run + 1))
    done
    run=1
    while [ "$run" -le "$RUNS" ]; do
      log "  $AGENT_B_NAME run $run/$RUNS"
      run_task_for_agent "$AGENT_B_NAME" "$AGENT_B_BIN" "$AGENT_B_CMD" "$task_file" "$run" "$order_index"
      order_index=$((order_index + 1))
      run=$((run + 1))
    done
  done < "$RUN_DIR/task-list.txt"
else
  while IFS= read -r task_file; do
    task_base="$(basename "$task_file" .md)"
    log "Task: $task_base"
    order_index=1
    run=1
    while [ "$run" -le "$RUNS" ]; do
      while IFS= read -r which; do
        if [ "$which" = "a" ]; then
          log "  $AGENT_A_NAME run $run/$RUNS"
          run_task_for_agent "$AGENT_A_NAME" "$AGENT_A_BIN" "$AGENT_A_CMD" "$task_file" "$run" "$order_index"
        else
          log "  $AGENT_B_NAME run $run/$RUNS"
          run_task_for_agent "$AGENT_B_NAME" "$AGENT_B_BIN" "$AGENT_B_CMD" "$task_file" "$run" "$order_index"
        fi
        order_index=$((order_index + 1))
      done < <(run_order_for_index "$run")
      run=$((run + 1))
    done
  done < "$RUN_DIR/task-list.txt"
fi

sample_idle_for_agent "$AGENT_A_NAME" "$AGENT_A_BIN" "$AGENT_A_IDLE_CMD"
sample_idle_for_agent "$AGENT_B_NAME" "$AGENT_B_BIN" "$AGENT_B_IDLE_CMD"

python3 - "$RUN_DIR" <<'PY'
import csv
import math
import os
import statistics
import sys
from collections import defaultdict

run_dir = sys.argv[1]
static_csv = os.path.join(run_dir, "static.csv")
runs_csv = os.path.join(run_dir, "runs.csv")
idle_csv = os.path.join(run_dir, "idle.csv")
summary_md = os.path.join(run_dir, "summary.md")

def rows(path):
    with open(path, newline="") as f:
        yield from csv.DictReader(f)

def fmt_float(v):
    if v is None or v == "":
        return ""
    return f"{float(v):.3f}"

def pct_delta(a, b):
    try:
        a = float(a)
        b = float(b)
        if b == 0:
            return ""
        return f"{((a - b) / b) * 100:+.1f}%"
    except Exception:
        return ""

static = list(rows(static_csv))
runs = list(rows(runs_csv))
idle = list(rows(idle_csv)) if os.path.exists(idle_csv) else []

by_task = defaultdict(list)
for r in runs:
    try:
        float(r["elapsed_sec"])
    except Exception:
        continue
    by_task[(r["agent"], r["task"])].append(r)

def metric(values, field):
    result = []
    for value in values:
        try:
            result.append(float(value[field]))
        except Exception:
            pass
    return result

agents = []
for r in static:
    if r["agent"] not in agents:
        agents.append(r["agent"])

with open(summary_md, "w") as f:
    f.write("# Agent Benchmark Summary\n\n")
    config_path = os.path.join(run_dir, "config.txt")
    order_mode = ""
    if os.path.exists(config_path):
        for line in open(config_path):
            if line.startswith("order_mode="):
                order_mode = line.strip().split("=", 1)[1]
                break
    if order_mode:
        f.write(f"Order mode: `{order_mode}`\n\n")
    f.write("## Static\n\n")
    f.write("| Agent | Binary | Exists | Size | Help exit | Help elapsed | Offline smoke |\n")
    f.write("|---|---:|---:|---:|---:|---:|---:|\n")
    for r in static:
        smoke = "n/a"
        if r.get("self_exit") or r.get("self_elapsed_sec"):
            smoke = f"{r.get('self_exit', '')} / {fmt_float(r.get('self_elapsed_sec', ''))}s"
        f.write(
            f"| {r['agent']} | `{r['resolved_bin']}` | {r['exists']} | "
            f"{r['size_human'] or r['size_bytes']} | {r.get('help_exit', '')} | "
            f"{fmt_float(r.get('help_elapsed_sec', ''))}s | {smoke} |\n"
        )

    f.write("\n## Task Runtime\n\n")
    f.write("| Agent | Task | Runs | Failures | Mean | Median | Min | Max | Stddev | Median CPU | Median RSS |\n")
    f.write("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
    for (agent, task), records in sorted(by_task.items()):
        vals = metric(records, "elapsed_sec")
        cpu = metric(records, "cpu_seconds")
        rss = metric(records, "max_rss_bytes")
        failures = sum(1 for r in runs if r["agent"] == agent and r["task"] == task and r["exit_code"] != "0")
        mean = statistics.mean(vals)
        median = statistics.median(vals)
        min_v = min(vals)
        max_v = max(vals)
        std = statistics.stdev(vals) if len(vals) > 1 else 0.0
        median_cpu = f"{statistics.median(cpu):.3f}s" if cpu else ""
        median_rss = f"{statistics.median(rss) / (1024 * 1024):.1f} MiB" if rss else ""
        f.write(
            f"| {agent} | {task} | {len(vals)} | {failures} | "
            f"{mean:.3f}s | {median:.3f}s | {min_v:.3f}s | {max_v:.3f}s | {std:.3f}s | "
            f"{median_cpu} | {median_rss} |\n"
        )

    if len(agents) >= 2:
        f.write("\n## Median Delta\n\n")
        f.write(f"Delta is `{agents[0]}` relative to `{agents[1]}`. Negative means faster/smaller.\n\n")
        f.write("| Task | " + agents[0] + " median | " + agents[1] + " median | Delta |\n")
        f.write("|---|---:|---:|---:|\n")
        tasks = sorted({task for _, task in by_task})
        for task in tasks:
            a_vals = metric(by_task.get((agents[0], task), []), "elapsed_sec")
            b_vals = metric(by_task.get((agents[1], task), []), "elapsed_sec")
            if not a_vals or not b_vals:
                continue
            a_med = statistics.median(a_vals)
            b_med = statistics.median(b_vals)
            f.write(f"| {task} | {a_med:.3f}s | {b_med:.3f}s | {pct_delta(a_med, b_med)} |\n")

    if idle:
        f.write("\n## Idle Samples\n\n")
        f.write("| Agent | Samples | Avg CPU % | Avg RSS KB |\n")
        f.write("|---|---:|---:|---:|\n")
        by_agent = defaultdict(list)
        for r in idle:
            try:
                by_agent[r["agent"]].append((float(r["cpu_percent"]), float(r["rss_kb"])))
            except Exception:
                pass
        for agent, vals in sorted(by_agent.items()):
            avg_cpu = statistics.mean(v[0] for v in vals)
            avg_rss = statistics.mean(v[1] for v in vals)
            f.write(f"| {agent} | {len(vals)} | {avg_cpu:.2f} | {avg_rss:.0f} |\n")

    f.write("\n## Caveats\n\n")
    f.write("- Use the same provider, model, API keys, and network conditions for both agents.\n")
    f.write("- The agents have different built-in system prompts; treat that as a product-level caveat.\n")
    f.write("- Context RAM is not generic unless both agents expose the same history import/export API.\n")
    f.write("- Raw stdout/stderr and time logs are in `raw/`; machine-readable data is in `*.csv`.\n")

print(summary_md)
PY

log "Done."
log "Summary: $RUN_DIR/summary.md"
