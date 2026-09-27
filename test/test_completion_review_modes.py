#!/usr/bin/env python3
"""Offline completion-gate tests using the real binary and shared SSE fixture."""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import termios
import json
import queue
import time
import subprocess
import sys
import threading
from pathlib import Path
from test_background_modes import fixture, answer, call, TIMEOUT

# Markers emitted by the Capstan binary. These must not drift from the runtime
# locations noted inline; the TUI assertions match against their rendered text.
REVIEWER_PROMPT = 'You are the independent completion reviewer'  # agent/completion_review.lua instruction
REVIEWER_PROMPT_SHORT = 'independent completion reviewer'        # derived from REVIEWER_PROMPT
STATUS_REVIEWING = 'Reviewing'                       # agent/completion_review.lua status()
STATUS_MULTIPLE = '(+1)'                             # agent/runtime.lua set_review_status()
MARKER_LAUNCH = 'Results will arrive later'          # agent/runtime.lua review_event()
MARKER_REQUEUE = 'requeued'                          # agent/runtime.lua review_event()
VERDICT_CLEAN = 'No open findings.'                  # agent/completion_review.lua finalize()
VERDICT_INCOMPLETE = 'Review incomplete:'            # agent/completion_review.lua stop()
VERDICT_EARLIER = 'Review completed for an earlier request.'  # agent/completion_review.lua stop()
STALE_NOTICE = 'Workspace changed outside'           # agent/completion_review.lua stop()
RESULT_COMPLETED_PREFIX = 'completed: '              # agent/completion_review.lua finalize()
REPAIR_GATE = 'without request_completion'           # agent/runtime.lua
REPAIR_ACCEPTANCE = 'acceptance is not confirmed'    # agent/completion_review.lua stop()
STATE_MARKER = 'Completion review runtime state'     # agent/runtime.lua
STATE_CONTINUE = 'continue the conversation without resubmitting'  # agent/runtime.lua
STATE_STAGE_REVIEWING = '"stage":"reviewing"'        # agent/completion_review.lua state()
MAX_TURNS_PREFIX = 'max agent turns exceeded: '      # agent/runtime.lua stop_run()
TUI_SHIFT_TAB = b'Shift+Tab'                         # src/tui.c

# Fixture-owned markers produced by the mock model, not the binary.
DRAFT = 'accepted draft'
REPAIRED = 'repaired explanation'
APPLYING_FIXES = 'Applying review fixes now.'
SECOND_DRAFT = 'second accepted'
CONVERSATION_REPLY = 'new request handled'
QUESTION = 'Which file?'
SUMMARY = 'Checked task and snapshot'


class ReviewScript:
    def __init__(self, case):
        self.case = case
        self.release = threading.Event()
        self.errors = []
        self.requests = []
        self.parent = 0
        self.reviews = 0
        self.reads = 0

    def reply(self, request):
        self.requests.append(request)
        messages = request['messages']
        reviewer = any(REVIEWER_PROMPT in str(m.get('content', ''))
                       for m in messages)
        if reviewer:
            names = {t['function']['name'] for t in request.get('tools', [])}
            assert names == {'file_read', 'submit_review'}, names
            results = [m for m in messages if m.get('role') == 'tool']
            if not results:
                self.reviews += 1
                return call('file_read', {'path': 'independent.txt'}, 'snapshot-read')
            assert 'independent-ok' in results[-1]['content'], results
            self.reads += 1
            if self.case == 'long_review' and self.reads < 7:
                return call('file_read', {'path': 'independent.txt'}, 'read-' + str(self.reads))
            if self.case == 'malformed':
                return answer('{}')
            findings, checks = [], []
            kind = 'clean'
            if self.case in ('repairs', 'repair_prose'):
                if self.reviews == 1:
                    kind = 'findings'
                    findings = [{'severity': 'high', 'description': 'Required explanation missing',
                                 'evidence': 'Task requires an explanation, draft omits it'}]
                else:
                    # Stable persisted issue ID is supplied to the re-review.
                    assert 'issue-1' in str(messages)
                    checks = [{'id': 'issue-1', 'status': 'resolved',
                               'evidence': 'Updated answer supplies the explanation'}]
            return answer(json.dumps({'verdict': kind, 'summary': SUMMARY,
                                      'findings': findings, 'checks': checks}))
        self.parent += 1
        if self.case == 'question':
            return call('request_completion', {'status': 'question', 'text': QUESTION}, 'done')
        if self.case == 'conversation':
            return answer(DRAFT)
        if self.case in ('tool_work', 'tool_completion', 'write_conversation'):
            if self.parent == 1:
                if self.case == 'write_conversation':
                    return call('file_write', {'path': 'note.txt', 'content': 'progress'}, 'parent-write')
                return call('file_read', {'path': 'independent.txt'}, 'parent-read')
            if self.case == 'tool_completion':
                return call('request_completion', {'status': 'ready', 'text': DRAFT}, 'done')
            return answer(DRAFT)
        if self.parent == 1:
            if self.case == 'clean':
                return call('request_completion', {'status': 'ready', 'text': DRAFT}, 'done')
            return call('request_completion', {'status': 'review', 'text': DRAFT}, 'done')
        assert self.case in ('repairs', 'repair_prose') and self.parent == 2, (self.case, self.parent)
        assert 'issue-1' in str(messages)
        if self.case == 'repair_prose':
            return answer(REPAIRED)
        return call('request_completion', {'status': 'review', 'text': REPAIRED}, 'repaired')


def cli(binary, case, max_turns=80):
    script = ReviewScript(case)
    with fixture(script=script, completion_review='true', git=True) as (_, workspace, env):
        proc = subprocess.run([str(binary), 'run', '--json', '--yolo', '--no-mcp', '--no-wiki',
                               '--max-turns', str(max_turns),
                               '--prompt', 'Explain the independent fixture file'],
                              cwd=workspace, env=env, stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=TIMEOUT)
        assert not script.errors, script.errors
        result = json.loads(proc.stdout)
        limited = case == 'long_review' and max_turns < 8
        if case in ('malformed', 'repair_prose') or limited:
            assert not result['ok'] and VERDICT_INCOMPLETE in result['text'], result
            if case == 'repair_prose':
                assert REPAIR_GATE in result['text'], result
                # Prose streams immediately, but cannot bypass the completion gate.
                assert REPAIR_ACCEPTANCE in result['text'], result
            if limited:
                assert MAX_TURNS_PREFIX + str(max_turns) in result['text'], result
        else:
            assert proc.returncode == 0 and result['ok'], (result, proc.stderr)
            if case in ('question', 'conversation', 'tool_work', 'tool_completion',
                        'write_conversation', 'clean'):
                expected = QUESTION if case == 'question' else DRAFT
                assert result['text'] == expected, result
            else:
                assert RESULT_COMPLETED_PREFIX + 'clean' in result['text'], result
        assert script.parent == (2 if case in ('repairs', 'repair_prose', 'tool_work', 'tool_completion', 'write_conversation') else 1), script.parent
        assert script.reviews == (0 if case in ('question', 'conversation', 'tool_work', 'tool_completion', 'write_conversation', 'clean') else 2 if case == 'repairs' else 1)
        assert script.reads == (min(7, max_turns - 1) if case == 'long_review' else script.reviews)
    print('CLI completion review ' + case + ': ok')


def cli_no_head(binary):
    script = ReviewScript('explicit')
    with fixture(script=script, completion_review='true') as (_, workspace, env):
        proc = subprocess.run([str(binary), 'run', '--json', '--yolo', '--no-mcp', '--no-wiki',
                               '--prompt', 'Explain the independent fixture file'],
                              cwd=workspace, env=env, stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=TIMEOUT)
        assert not script.errors, script.errors
        result = json.loads(proc.stdout)
        assert not result['ok'] and VERDICT_INCOMPLETE in result['text'], result
        assert 'HEAD baseline unavailable' in result['text'], result
        assert script.parent == 1 and script.reviews == 0, (script.parent, script.reviews)
    print('CLI completion review without HEAD: ok')


def acp(binary, cancel):
    script = ReviewScript('explicit')
    waiting = threading.Event()
    original_reply = script.reply

    def delayed(request):
        events = original_reply(request)
        if script.reads:
            waiting.set()
            assert script.release.wait(TIMEOUT), 'review not released'
        return events

    script.reply = delayed
    with fixture(script=script, completion_review='true', git=True) as (_, workspace, env):
        # Snapshot permissions never prompt: grant persistent read access via yolo.
        proc = subprocess.Popen([str(binary), 'acp', '--yolo'], cwd=workspace, env=env,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        inbox, errors, updates = queue.Queue(), [], []
        threading.Thread(target=lambda: [inbox.put(line) for line in proc.stdout], daemon=True).start()
        threading.Thread(target=lambda: errors.extend(proc.stderr), daemon=True).start()

        def send(ident, method, params):
            message = {'jsonrpc': '2.0', 'method': method, 'params': params}
            if ident is not None:
                message['id'] = ident
            proc.stdin.write(json.dumps(message) + '\n')
            proc.stdin.flush()

        def receive(ident):
            deadline = time.monotonic() + TIMEOUT
            while time.monotonic() < deadline:
                msg = json.loads(inbox.get(timeout=max(.01, deadline - time.monotonic())))
                if msg.get('id') == ident:
                    assert 'error' not in msg, (msg, errors)
                    return msg['result']
                updates.append(msg)
            raise AssertionError(errors)

        try:
            send(1, 'initialize', {'protocolVersion': 1, 'clientCapabilities': {}})
            receive(1)
            send(2, 'session/new', {'cwd': str(workspace), 'mcpServers': []})
            session = receive(2)['sessionId']
            send(3, 'session/prompt', {'sessionId': session, 'prompt': [
                {'type': 'text', 'text': 'Explain the independent fixture file'}]})
            assert waiting.wait(TIMEOUT), (script.errors, errors)
            # Reviewer is outstanding: neither the verdict nor end_turn may escape.
            time.sleep(.1)
            while not inbox.empty():
                msg = json.loads(inbox.get_nowait())
                assert msg.get('id') != 3, msg
                updates.append(msg)
            assert RESULT_COMPLETED_PREFIX + 'clean' not in json.dumps(updates), updates
            if cancel:
                send(None, 'session/cancel', {'sessionId': session})
            else:
                script.release.set()
            result = receive(3)
            assert result['stopReason'] == ('cancelled' if cancel else 'end_turn'), result
            if cancel:
                assert RESULT_COMPLETED_PREFIX + 'clean' not in json.dumps(updates), updates
            else:
                assert RESULT_COMPLETED_PREFIX + 'clean' in json.dumps(updates), updates
            assert script.parent == 1 and script.reviews == 1
            send(4, 'session/close', {'sessionId': session})
            receive(4)
            proc.stdin.close()
            assert proc.wait(timeout=5) == 0, errors
        finally:
            script.release.set()
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)
    print('ACP completion review ' + ('cancel' if cancel else 'held answer') + ': ok')


def tui(binary, concurrent=False, repair=False):
    script = ReviewScript('repairs' if concurrent == 'findings' or repair else 'explicit')
    waiting = threading.Event()
    second_waiting, second_release = threading.Event(), threading.Event()
    second_finished = threading.Event()
    reply_text = SECOND_DRAFT if concurrent == 'review' else CONVERSATION_REPLY
    original_reply = script.reply

    def delayed(request):
        reviewer = REVIEWER_PROMPT_SHORT in str(request['messages'])
        if reviewer and concurrent == 'review' and SECOND_DRAFT in str(request['messages']):
            events = original_reply(request)
            if any(m.get('role') == 'tool' for m in request['messages']):
                second_waiting.set()
                assert second_release.wait(TIMEOUT), 'second review not released'
                second_finished.set()
            return events
        if concurrent and not reviewer and script.parent >= 1:
            state_messages = [m['content'] for m in request['messages']
                              if m.get('role') == 'system' and
                              STATE_MARKER in m.get('content', '')]
            assert len(state_messages) == 1, 'missing or duplicate review context'
            assert STATE_STAGE_REVIEWING in state_messages[0]
            assert STATE_CONTINUE in state_messages[0]
            script.parent += 1
            if concurrent == 'tools' and script.parent == 2:
                return call('file_read', {'path': 'independent.txt'}, 'conversation-read')
            if concurrent == 'write':
                if script.parent == 2:
                    return call('file_write', {'path': 'independent.txt', 'content': 'new contents'}, 'edit')
                assert 'new contents' in str(request['messages'])
            if concurrent in ('write', 'review'):
                return call('request_completion', {'status': 'review' if concurrent == 'review' else 'question',
                                                   'text': reply_text}, 'done')
            return answer(reply_text)
        events = original_reply(request)
        if repair and not reviewer and script.parent == 2:
            events.insert(0, {'choices': [{'delta': {'content': APPLYING_FIXES}}]})
        if reviewer and script.reads:
            waiting.set()
            assert script.release.wait(TIMEOUT), 'review not released'
        return events

    script.reply = delayed
    with fixture(script=script, completion_review='true', git=True) as (_, workspace, env):
        env['LANG'] = 'en_US.UTF-8'
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 100, 0, 0))
        proc = subprocess.Popen([str(binary), '--yolo', '--no-mcp', '--no-wiki'],
                                cwd=workspace, env=env, stdin=slave, stdout=slave,
                                stderr=slave, start_new_session=True)
        os.close(slave)
        screen = bytearray()
        ansi = re.compile(rb'\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*\x07|[()][A-Z0-9]|[=>])')

        def until(predicate):
            deadline = time.monotonic() + TIMEOUT
            while time.monotonic() < deadline:
                if select.select([master], [], [], .02)[0]:
                    screen.extend(os.read(master, 65536))
                assert not script.errors, script.errors
                assert proc.poll() is None, bytes(screen[-2000:])
                if predicate():
                    return
            raise AssertionError(bytes(screen[-3000:]))

        def plain(start=0):
            return ansi.sub(b'', bytes(screen[start:])).decode('utf-8', 'replace')

        try:
            until(lambda: TUI_SHIFT_TAB in screen)
            os.write(master, b'Explain the independent fixture file\r')
            until(lambda: waiting.is_set() and STATUS_REVIEWING in plain())
            # Explicit review never streams its scope text; only the verdict is held.
            assert DRAFT not in plain(), 'review scope text must not stream'
            assert VERDICT_CLEAN not in plain()
            # Narrow terminal + resize must preserve the active indicator.
            start = len(screen)
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 20, 36, 0, 0))
            proc.send_signal(signal.SIGWINCH)
            until(lambda: 'Review' in plain(start))  # status may be clipped at narrow width
            if concurrent:
                os.write(master, b'Change independent.txt now\r' if concurrent == 'write' else b'How is it going?\r')
                if concurrent == 'review':
                    # Snapshot capture conservatively waits for managed jobs;
                    # the second review is queued while the first is running.
                    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 0, 0))
                    proc.send_signal(signal.SIGWINCH)
                    until(lambda: STATUS_MULTIPLE in plain())
                    script.release.set()
                    until(lambda: second_waiting.is_set())
                    # First owner completed; the second review remains active.
                    redraw = len(screen)
                    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 60, 120, 0, 0))
                    proc.send_signal(signal.SIGWINCH)
                    until(lambda: STATUS_REVIEWING in plain(redraw))
                    assert STATUS_MULTIPLE not in plain(redraw), 'first owner was not removed'
                    second_release.set()
                    until(lambda: second_finished.is_set())
                    # The accumulated PTY buffer keeps old status frames, so
                    # poll fresh full redraws (alternating sizes forces a
                    # repaint) until finalize clears the live-review indicator.
                    sizes = ((61, 121), (62, 121))
                    redraw = len(screen)
                    deadline = time.monotonic() + TIMEOUT
                    while True:
                        fcntl.ioctl(master, termios.TIOCSWINSZ,
                                    struct.pack('HHHH', sizes[0][0], sizes[0][1], 0, 0))
                        sizes = (sizes[1], sizes[0])
                        proc.send_signal(signal.SIGWINCH)
                        painted = time.monotonic() + TIMEOUT
                        while time.monotonic() < painted:
                            if select.select([master], [], [], .02)[0]:
                                screen.extend(os.read(master, 65536))
                            assert not script.errors, script.errors
                            assert proc.poll() is None, bytes(screen[-2000:])
                            if STATUS_REVIEWING not in plain(redraw):
                                break
                        if STATUS_REVIEWING not in plain(redraw):
                            break
                        if time.monotonic() >= deadline:
                            raise AssertionError(
                                'full=' + plain() + '\n\nredraw=' + plain(redraw) +
                                '\nreviews=%d reads=%d parent=%d requests=%d' % (
                                    script.reviews, script.reads, script.parent, len(script.requests)))
                        redraw = len(screen)
                    assert script.parent == 2 and script.reviews == 2
                    return
                until(lambda: reply_text in plain())
                if concurrent == 'write':
                    assert (workspace / 'independent.txt').read_text() == 'new contents'
                assert not script.release.is_set(), 'new request waited for review'
                redraw = len(screen)
                fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 31, 101, 0, 0))
                proc.send_signal(signal.SIGWINCH)
                until(lambda: reply_text in plain(redraw))
                # The live-review indicator persists across resizes, but ncurses
                # only re-emits changed cells: an unchanged row-0 status may be
                # absent from the PTY diff. Assert against the full transcript.
                assert STATUS_REVIEWING in plain(), ('background review indicator disappeared', plain())
                assert STATUS_MULTIPLE not in plain(), 'completed review left a stale status'
                assert script.reviews == (2 if concurrent == 'review' else 1), 'unexpected review count'
                script.release.set()
                # The draft streams at submission; the verdict arrives later as
                # a new notification, never by rewriting the originating answer.
                fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 60, 120, 0, 0))
                proc.send_signal(signal.SIGWINCH)
                verdict = (VERDICT_INCOMPLETE if concurrent == 'write' else
                           VERDICT_EARLIER if concurrent == 'findings' else
                           VERDICT_CLEAN)
                if concurrent == 'findings':
                    # Foreground was transferred to the newer request, so the
                    # choice modal asks fix-now vs ask-later before repairs
                    # can defer to /issues.
                    until(lambda: 'Ask later' in plain())
                    os.write(master, b'2')
                until(lambda: verdict in plain())
                if concurrent == 'write':
                    assert VERDICT_CLEAN not in plain(), 'stale draft accepted'
                else:
                    assert VERDICT_INCOMPLETE not in plain()
                    assert STALE_NOTICE not in plain()
                    # Incremental terminal updates are not message order. Force
                    # a full redraw before checking chronological notification order.
                    redraw = len(screen)
                    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 61, 121, 0, 0))
                    proc.send_signal(signal.SIGWINCH)
                    until(lambda: verdict in plain(redraw) and reply_text in plain(redraw))
                    transcript = plain(redraw)
                    assert 0 <= transcript.find(MARKER_LAUNCH) < transcript.find(reply_text) < transcript.find(verdict), 'result rewrote history'
                    if concurrent != 'findings':
                        assert STATUS_REVIEWING not in plain(redraw), 'finished review left its indicator'
                assert script.parent == (3 if concurrent in ('write', 'tools') else 2)
                assert script.reviews == (2 if concurrent == 'review' else 1)
                return
            script.release.set()
            until(lambda: VERDICT_CLEAN in plain())
            start = len(screen)
            fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 60, 120, 0, 0))
            proc.send_signal(signal.SIGWINCH)
            until(lambda: VERDICT_CLEAN in plain(start))
            assert STATUS_REVIEWING not in plain(start), plain(start)
            assert script.parent == (2 if repair else 1) and script.reviews == (2 if repair else 1)
            if repair:
                # Incremental PTY updates are not message order. Force a single
                # full redraw, then check chronological notification order.
                redraw = len(screen)
                fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack('HHHH', 61, 121, 0, 0))
                proc.send_signal(signal.SIGWINCH)
                until(lambda: MARKER_LAUNCH in plain(redraw) and APPLYING_FIXES in plain(redraw)
                             and MARKER_REQUEUE in plain(redraw))
                transcript = plain(redraw)
                # The PTY buffer may retain an earlier partial frame; verify the
                # chronological order only inside the last full repaint.
                base = transcript.rfind('Explain the independent fixture file')
                transcript = transcript[base:]
                assert 0 <= transcript.find(MARKER_LAUNCH) < transcript.find(APPLYING_FIXES), transcript
                assert transcript.find(APPLYING_FIXES) < transcript.find(MARKER_REQUEUE), transcript
        finally:
            second_release.set()
            script.release.set()
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)
            os.close(master)
    print('TUI completion review indicator/resize/held answer/cleanup: ok')


if __name__ == '__main__':
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/capstan').resolve()
    for case in ('clean', 'explicit', 'question', 'conversation', 'tool_work', 'tool_completion',
                 'write_conversation', 'malformed', 'repairs', 'repair_prose'):
        cli(binary, case)
    cli(binary, 'long_review', max_turns=9)
    cli(binary, 'long_review', max_turns=6)
    cli_no_head(binary)
    for cancel in (False, True):
        acp(binary, cancel)
    tui(binary)
    tui(binary, repair=True)
    for concurrent in ('question', 'tools', 'findings', 'write', 'review'):
        tui(binary, concurrent=concurrent)
        print('TUI concurrent ' + concurrent + ' during immutable review: ok')
