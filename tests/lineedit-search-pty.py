#!/usr/bin/env python3
"""Exercise history search on a PTY, including the terminal cursor position."""

import codecs
import curses
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time
import unicodedata


class Screen:
    """The xterm operations used by the line editor, with delayed wrapping."""

    def __init__(self, columns, rows):
        self.columns = columns
        self.rows = rows
        self.row = self.column = 0
        self.pending_wrap = False
        self.cells = {}
        self.scrolls = 0
        self.state = "text"
        self.sequence = ""
        self.decoder = codecs.getincrementaldecoder("utf-8")()
        self.bells = []
        self.bell_scrolls = []

    def feed(self, data):
        for c in self.decoder.decode(data):
            if self.state == "escape":
                if c == "[":
                    self.state, self.sequence = "csi", ""
                elif c in "()":
                    self.state = "charset"
                elif c == "]":
                    self.state = "osc"
                else:
                    self.state = "text"
                continue
            if self.state == "charset":
                self.state = "text"
                continue
            if self.state == "osc":
                if c == "\a":
                    self.state = "text"
                elif c == "\x1b":
                    self.state = "osc-escape"
                continue
            if self.state == "osc-escape":
                self.state = "text" if c == "\\" else "osc"
                continue
            if self.state == "csi":
                self.sequence += c
                if "@" <= c <= "~":
                    self.control(self.sequence)
                    self.state = "text"
                continue
            if c == "\x1b":
                self.state = "escape"
            elif c == "\a":
                self.bells.append((self.row, self.column, dict(self.cells)))
                self.bell_scrolls.append(self.scrolls)
            elif c == "\r":
                self.column, self.pending_wrap = 0, False
            elif c == "\n":
                self.next_row()
                self.pending_wrap = False
            elif c == "\b":
                self.column = max(0, self.column - 1)
                self.pending_wrap = False
            elif c >= " " and c != "\x7f":
                self.put(c)

    def control(self, sequence):
        final, params = sequence[-1], sequence[:-1]
        if params.startswith(("?", ">")):
            return
        try:
            numbers = [int(n) if n else 0 for n in params.split(";")]
        except ValueError:
            return
        count = numbers[0] or 1
        if final == "A":
            self.row = max(0, self.row - count)
        elif final == "B":
            self.row = min(self.rows - 1, self.row + count)
        elif final == "C":
            self.column = min(self.columns - 1, self.column + count)
        elif final == "D":
            self.column = max(0, self.column - count)
        elif final in "Hf":
            self.row = min(self.rows - 1, max(0, numbers[0] - 1))
            self.column = max(0, (numbers[1] if len(numbers) > 1 else 1) - 1)
        elif final == "J":
            mode = numbers[0]
            if mode == 2:
                self.cells.clear()
            elif mode == 0:
                self.cells = {p: v for p, v in self.cells.items()
                              if p < (self.row, self.column)}
        elif final == "K":
            mode = numbers[0]
            if mode == 0:
                self.cells = {p: v for p, v in self.cells.items()
                              if p[0] != self.row or p[1] < self.column}
            elif mode == 2:
                self.cells = {p: v for p, v in self.cells.items()
                              if p[0] != self.row}
        if final in "ABCDHf":
            self.pending_wrap = False

    def put(self, c):
        width = 2 if unicodedata.east_asian_width(c) in "FW" else 1
        if self.pending_wrap or self.column + width > self.columns:
            self.next_row()
            self.column = 0
            self.pending_wrap = False
        self.cells[self.row, self.column] = c
        if width == 2:
            self.cells[self.row, self.column + 1] = ""
        if self.column + width == self.columns:
            self.column = self.columns - 1
            self.pending_wrap = True
        else:
            self.column += width

    def next_row(self):
        if self.row < self.rows - 1:
            self.row += 1
        else:
            self.cells = {(row - 1, column): value
                          for (row, column), value in self.cells.items()
                          if row > 0}
            self.scrolls += 1


class Session:
    def __init__(self, executable, columns, rows=200):
        self.columns = columns
        self.screen = Screen(columns, rows)
        self.temp = tempfile.TemporaryDirectory(prefix="yash-search-")
        rcfile = os.path.join(self.temp.name, "rc")
        with open(rcfile, "w", encoding="utf-8") as rc:
            rc.write("PS1='P> ' PS1R= PS1S= PS1P=\n"
                     "unset HISTFILE\n"
                     "set -o emacs -o lenoconvmeta +o lepredict +o lepromptsp\n"
                     "bindkey -e '\\^T' alert\n"
                     "trap 'LC_ALL=C; printf \"\\007\" >&2' USR1\n"
                     "trap 'LC_ALL=C.UTF-8; printf \"\\007\" >&2' USR2\n")
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ,
                        struct.pack("HHHH", rows, columns, 0, 0))
            env = {"PATH": os.defpath, "HOME": self.temp.name,
                   "TERM": "xterm", "LC_ALL": "C.UTF-8",
                   "YASH_LOADPATH": ""}
            os.execve(executable, [executable, "--noprofile",
                                  "--rcfile=" + rcfile, "-i"], env)
        try:
            curses.setupterm("xterm", self.master)
            self.left = curses.tigetstr("kcub1")
            self.right = curses.tigetstr("kcuf1")
            self.read_until(lambda: any(
                self.line(row).startswith("P> ")
                for row in range(self.screen.row + 1)))
            self.send(b"\x14")
            self.bell()
        except BaseException:
            self.close()
            raise

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(self.pid, 0)
        os.close(self.master)
        self.temp.cleanup()

    def line(self, row, cells=None):
        if cells is None:
            cells = self.screen.cells
        return "".join(cells.get((row, column), " ")
                       for column in range(self.columns))

    def read_until(self, predicate):
        deadline = time.monotonic() + 10
        while not predicate():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError("timed out waiting for terminal event\n" +
                                     self.dump())
            ready, _, _ = select.select([self.master], [], [], remaining)
            if not ready:
                raise AssertionError("timed out waiting for terminal event\n" +
                                     self.dump())
            data = os.read(self.master, 65536)
            if not data:
                raise AssertionError("shell closed the terminal\n" + self.dump())
            self.screen.feed(data)

    def dump(self, cells=None):
        if cells is None:
            cells = self.screen.cells
        rows = sorted({row for row, _ in cells})
        return "\n".join("%d: %r" % (r, self.line(r, cells)) for r in rows)

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        os.write(self.master, data)

    def bell(self):
        # An unbound control key alerts without changing the search.  Its bell
        # follows the preceding complete redraw, so it is a frame boundary.
        self.read_until(lambda: bool(self.screen.bells))
        self.last_bell_scrolls = self.screen.bell_scrolls.pop(0)
        return self.screen.bells.pop(0)

    def checkpoint(self, data=b""):
        self.send(data + b"\x14")
        return self.bell()

    def history(self, text):
        snapshot = self.checkpoint(text.encode("utf-8") + b"\r")
        row, column, cells = snapshot
        assert self.line(row, cells).startswith("P> ") and column == 3, (
            "command did not reach a new prompt\n" + self.dump(cells))

    def search(self, text):
        return self.checkpoint(b"\x12" + text.encode("utf-8"))

    def assert_cursor(self, snapshot, line, offset):
        row, column, cells = snapshot
        prompt_rows = [r for r in {p[0] for p in cells}
                       if self.line(r, cells).startswith("P> ")]
        assert prompt_rows, self.dump(cells)
        expected_row, expected_column = max(prompt_rows), 3
        for c in line[:offset]:
            width = 2 if unicodedata.east_asian_width(c) in "FW" else 1
            if expected_column + width > self.columns:
                expected_row, expected_column = expected_row + 1, 0
            assert cells.get((expected_row, expected_column)) == c, (
                f"expected {c!r} at {(expected_row, expected_column)}\n" +
                self.dump(cells))
            expected_column += width
            if expected_column == self.columns:
                expected_row, expected_column = expected_row + 1, 0
        if offset < len(line):
            width = 2 if unicodedata.east_asian_width(line[offset]) in "FW" else 1
            if expected_column + width > self.columns:
                expected_row, expected_column = expected_row + 1, 0
            assert cells.get((expected_row, expected_column)) == line[offset], (
                f"expected match character {line[offset]!r}\n" + self.dump(cells))
        expected = (expected_row, expected_column)
        assert (row, column) == expected, (
            f"cursor {(row, column)}, expected {expected}\n" + self.dump(cells))

    def assert_search_cursor(self, snapshot, query):
        row, column, cells = snapshot
        text = "Backward search: " + query
        rows = [r for r in {p[0] for p in cells} if text in self.line(r, cells)]
        assert rows, self.dump(cells)
        expected = self.line(rows[-1], cells).index(text) + len(text)
        assert (row, column) == (rows[-1], expected), self.dump(cells)


def run(executable, scenario):
    width = (12 if scenario in ("wide-wrap", "exact-wrap", "control-wrap", "narrow-query")
             else 30 if scenario.startswith("scroll") or scenario in ("one-row", "prompt-restore", "long-accept")
             else 40)
    rows = (1 if scenario == "one-row" else 2 if scenario == "narrow-query"
            else 4 if scenario.startswith("scroll") or scenario in ("prompt-restore", "long-accept")
            else 200)
    session = Session(executable, width, rows)
    try:
        if scenario in ("basic", "left", "right"):
            entry = ": alpha needle omega needle"
            match = entry.index("needle")
            session.history(entry)
            session.assert_cursor(session.search("needle"), entry, match)
            action = {"basic": b"\n", "left": session.left,
                      "right": session.right}[scenario]
            offset = match + {"basic": 0, "left": -1, "right": 1}[scenario]
            session.assert_cursor(session.checkpoint(action), entry, offset)
            changed = entry[:offset] + "X" + entry[offset:]
            session.assert_cursor(session.checkpoint(b"X"), changed, offset + 1)
        elif scenario == "prediction-navigation":
            entry = ": alpha beta gamma"
            session.history(entry)
            session.history("set -o lepredict")
            session.assert_cursor(session.checkpoint(b": alpha"), entry, 7)
            session.assert_cursor(session.checkpoint(session.right), entry, 8)
            session.assert_cursor(session.checkpoint(session.left), entry, 7)
            session.assert_cursor(session.checkpoint(session.right), entry, 8)
            session.assert_cursor(session.checkpoint(curses.tigetstr("kend")),
                                  entry, len(entry))
            session.assert_cursor(session.checkpoint(session.left), entry, len(entry) - 1)
            changed = entry[:-1] + "X" + entry[-1]
            session.assert_cursor(session.checkpoint(b"X"), changed, len(entry))
        elif scenario == "tab-completion":
            path = os.path.join(session.temp.name, "completion-target")
            with open(path, "w", encoding="utf-8"):
                pass
            line = ": " + path + " "
            session.assert_cursor(session.checkpoint((": " + path[:-6]).encode() + b"\t"),
                                  line, len(line))
        elif scenario == "wide-wrap":
            entry = ": 123456界tail"
            match = entry.index("界")
            session.history(entry)
            session.assert_cursor(session.search("界"), entry, match)
            session.assert_cursor(session.checkpoint(b"\n"), entry, match)
            changed = entry[:match] + "X" + entry[match:]
            session.assert_cursor(session.checkpoint(b"X"), changed, match + 1)
        elif scenario == "exact-wrap":
            entry = ": 1234567needle"
            match = entry.index("needle")
            session.history(entry)
            session.assert_cursor(session.search("needle"), entry, match)
            session.assert_cursor(session.checkpoint(b"\n"), entry, match)
        elif scenario == "unicode":
            entry = ": é界 alpha needle omega"
            match = entry.index("needle")
            session.history(entry)
            session.assert_cursor(session.search("needle"), entry, match)
            session.assert_cursor(session.checkpoint(b"\n"), entry, match)
        elif scenario == "repeat":
            older, newer = ": old needle tail", ": newer prefix needle tail"
            session.history(older)
            session.history(newer)
            session.assert_cursor(session.search("needle"), newer, newer.index("needle"))
            session.assert_cursor(session.checkpoint(b"\x12"), older, older.index("needle"))
            session.assert_cursor(session.checkpoint(b"\x13"), newer, newer.index("needle"))
        elif scenario == "mutate":
            entry = ": alpha needle nudge"
            session.history(entry)
            session.assert_cursor(session.search("needle"), entry, entry.index("needle"))
            session.assert_cursor(session.checkpoint(b"\x15nudge"), entry, entry.index("nudge"))
            session.assert_cursor(session.checkpoint(b"\x7f"), entry, entry.index("nudge"))
        elif scenario == "literal":
            entry = ": prefix *? suffix"
            session.history(entry)
            session.assert_cursor(session.search("*?"), entry, entry.index("*?"))
            session.assert_cursor(session.checkpoint(b"\n"), entry, entry.index("*?"))
        elif scenario == "control":
            rendered = ": a^Ineedle"
            session.checkpoint(b": a\x16\tneedle\r")
            session.assert_cursor(session.search("needle"), rendered, rendered.index("needle"))
            session.assert_cursor(session.checkpoint(b"\n"), rendered, rendered.index("needle"))
        elif scenario == "control-wrap":
            rendered = ": 1234567^Ineedle"
            session.checkpoint(b": 1234567\x16\tneedle\r")
            session.assert_cursor(session.checkpoint(b"\x12\x16\t"),
                                  rendered, rendered.index("^I"))
            session.assert_cursor(session.checkpoint(b"\n"),
                                  rendered, rendered.index("^I"))
        elif scenario == "combining":
            rendered = ": e<301>needle"
            session.history(": e\u0301needle")
            session.assert_cursor(session.search("\u0301"), rendered,
                                  rendered.index("<301>"))
            session.assert_cursor(session.checkpoint(b"\n"), rendered,
                                  rendered.index("<301>"))
        elif scenario == "undo-entry":
            entry, newer = ": alpha needle tail", ": other history"
            session.history(entry)
            session.history(newer)
            session.checkpoint(b"\x10\x10\x01XX\x0e")
            edited = "XX" + entry
            session.assert_cursor(session.search("needle"), edited, edited.index("needle"))
            session.assert_cursor(session.checkpoint(b"\n"), edited, edited.index("needle"))
        elif scenario == "undo-dirty":
            session.history(": alpha needle tail")
            session.history(": other history")
            session.checkpoint(b"\x10\x10\x01XX\x0e")
            session.checkpoint(b"YY")
            session.assert_search_cursor(session.search("XX"), "XX")
            session.send(b"\n\x14")
            session.bell()
            session.assert_cursor(session.bell(), ": other historyYY",
                                  len(": other historyYY"))
            session.assert_cursor(session.search("needle"), ": alpha needle tail",
                                  len(": alpha "))
            session.assert_cursor(session.checkpoint(b"\n"), ": alpha needle tail",
                                  len(": alpha "))
        elif scenario in ("undo-missing", "undo-split"):
            entry, newer = ": alpha needle tail", ": other history"
            session.history(entry)
            session.history(newer)
            if scenario == "undo-missing":
                edit = b"\x04"
                edited = ": alpha eedle tail"
                position = len(": alpha ")
            else:
                edit = b"X"
                edited = ": alpha neeXdle tail"
                position = len(": alpha nee")
            session.checkpoint(b"\x10\x10\x01" + b"\x06" * position + edit)
            session.checkpoint(b"\x0e")
            session.assert_search_cursor(session.search("needle"), "needle")
            session.send(session.right + b"\x14")
            session.bell()  # no matching edited history entry
            session.assert_cursor(session.bell(), newer, len(newer))
            session.assert_cursor(session.checkpoint(b"\x10"), edited,
                                  position + (scenario == "undo-split"))
            session.assert_cursor(session.checkpoint(b"X"), edited[:position +
                (scenario == "undo-split")] + "X" + edited[position +
                (scenario == "undo-split"):],
                position + (scenario == "undo-split") + 1)
        elif scenario in ("scroll", "scroll-late", "scroll-long-query"):
            entry = (": needle " + "a" * 90 if scenario != "scroll-late"
                     else ": " + "a" * 90 + " needle tail")
            query = "a" * 40 if scenario == "scroll-long-query" else "needle"
            session.history(entry)
            assert session.screen.scrolls > 0, "test did not reach the screen bottom"
            for keys in (b"\x12" + query.encode(), b"\x7f", query[-1:].encode()):
                row, column, cells = session.checkpoint(keys)
                assert 0 <= row < 4 and cells.get((row, column)) == query[0], (
                    "search match is not under the cursor\n" + session.dump(cells))
                assert any("Backward search: " in session.line(r, cells)
                           for r in range(4)), session.dump(cells)
                if keys.startswith(b"\x12"):
                    first_search_scrolls = session.last_bell_scrolls
                else:
                    assert session.last_bell_scrolls == first_search_scrolls, (
                        "search redraw scrolled the screen")
            row, column, cells = session.checkpoint(session.right)
            assert cells.get((row, column)) == query[1], session.dump(cells)
            row, column, cells = session.checkpoint(b"X")
            assert cells.get((row, column)) == query[1], session.dump(cells)
        elif scenario == "one-row":
            session.history(": needle " + "a" * 90)
            row, column, cells = session.search("needle")
            assert row == 0 and cells.get((row, column)) == "n", session.dump(cells)
            row, column, cells = session.checkpoint(b"\x7f")
            assert row == 0 and cells.get((row, column)) == "n", session.dump(cells)
        elif scenario == "scroll-draft":
            session.history(": old needle tail")
            session.checkpoint(b"x" * 100)
            assert session.screen.scrolls > 0, "test did not reach the screen bottom"
            row, column, cells = session.search("needle")
            assert cells.get((row, column)) == "n", session.dump(cells)
            row, column, cells = session.checkpoint(b"\x7f")
            assert cells.get((row, column)) == "n", session.dump(cells)
            row, column, cells = session.checkpoint(b"\x07")
            assert cells.get((row, column - 1)) == "x", session.dump(cells)
        elif scenario == "scroll-no-match":
            session.history(": needle " + "a" * 90)
            row, column, cells = session.search("needle")
            assert cells.get((row, column)) == "n", session.dump(cells)
            row, column, cells = session.checkpoint(b"missing")
            assert "Backward search: " in session.line(row, cells), session.dump(cells)
            row, column, cells = session.checkpoint(b"\x15needle")
            assert cells.get((row, column)) == "n", session.dump(cells)
        elif scenario == "prompt-restore":
            session.history(": needle " + "a" * 90)
            session.search("needle")
            session.assert_cursor(session.checkpoint(b"\x07"), "", 0)
            session.assert_cursor(session.checkpoint(b"echo"), "echo", 4)
        elif scenario == "long-accept":
            entry = ": needle " + "a" * 200
            session.history(entry)
            row, column, cells = session.search("needle")
            assert cells.get((row, column)) == "n", session.dump(cells)
            row, column, cells = session.checkpoint(session.right)
            assert cells.get((row, column)) == "e", session.dump(cells)
            row, column, cells = session.checkpoint(b"X")
            assert cells.get((row, column)) == "e", session.dump(cells)
            assert cells.get((row, column - 1)) == "X", session.dump(cells)
            row, column, cells = session.checkpoint(b"\x7f")
            assert cells.get((row, column)) == "e", session.dump(cells)
        elif scenario == "narrow-query":
            session.history(": needle tail")
            for keys, expected in ((b"\x12needle", "needle"),
                                   (b"missing", "missing")):
                row, column, cells = session.checkpoint(keys)
                assert any(expected in session.line(r, cells) for r in range(2)), (
                    f"query {expected!r} is not visible\n" + session.dump(cells))
        elif scenario == "empty":
            session.history(": previous history")
            session.checkpoint(b"draft")
            session.assert_search_cursor(session.checkpoint(b"\x12"), "")
            session.assert_search_cursor(session.checkpoint(b"\x12"), "")
            session.send(session.right + b"\x14")
            session.bell()  # accepting an empty search alerts
            session.assert_cursor(session.bell(), "draft", len("draft"))
        elif scenario in ("repeat-empty", "repeat-empty-accept"):
            older, newer = ": older needle", ": newer needle"
            session.history(older)
            session.history(newer)
            session.assert_cursor(session.search("needle"), newer, newer.index("needle"))
            session.assert_cursor(session.checkpoint(b"\x12"), older, older.index("needle"))
            session.assert_search_cursor(session.checkpoint(b"\x15"), "")
            if scenario == "repeat-empty":
                session.assert_cursor(session.checkpoint(b"needle"),
                                      older, older.index("needle"))
                session.assert_cursor(session.checkpoint(b"\n"),
                                      older, older.index("needle"))
            else:
                session.send(session.right + b"\x14")
                session.bell()
                session.assert_cursor(session.bell(), newer, len(newer))
        elif scenario == "invalid-history":
            session.history(": older needle")
            session.history(": é needle")
            session.history("LC_ALL=C")
            session.assert_cursor(session.search("needle"), ": older needle",
                                  len(": older "))
            session.assert_cursor(session.checkpoint(b"\n"), ": older needle",
                                  len(": older "))
        elif scenario in ("invalid-redraw", "invalid-enter"):
            session.history(": é needle")
            session.checkpoint(b"draft")
            session.assert_cursor(session.search("needle"), ": é needle",
                                  len(": é "))
            os.kill(session.pid, signal.SIGUSR1)
            session.bell()  # trap reports the locale change
            session.assert_search_cursor(session.checkpoint(), "needle")
            os.kill(session.pid, signal.SIGUSR2)
            session.bell()
            session.assert_cursor(session.checkpoint(), ": é needle", len(": é "))
            os.kill(session.pid, signal.SIGUSR1)
            session.bell()
            session.assert_search_cursor(session.checkpoint(), "needle")
            action = session.right if scenario == "invalid-redraw" else b"\r"
            session.send(action + b"\x14")
            session.bell()  # the invalidated result cannot be accepted
            session.assert_cursor(session.bell(), "draft", len("draft"))
        elif scenario in ("no-match-left", "no-match-right"):
            session.history(": history entry")
            session.checkpoint(b"draft")
            row, column, cells = session.search("not-present")
            rows = [r for r in {p[0] for p in cells}
                    if "Backward search: not-present" in session.line(r, cells)]
            assert rows, session.dump(cells)
            expected = session.line(rows[-1], cells).index("Backward search: not-present")
            assert (row, column) == (rows[-1], expected + len("Backward search: not-present"))
            arrow = session.left if scenario == "no-match-left" else session.right
            session.send(arrow + b"\x14")
            session.bell()  # the unsuccessful acceptance alerts
            session.assert_cursor(session.bell(), "draft", len("draft"))
        else:
            raise ValueError(scenario)
    finally:
        session.close()


if __name__ == "__main__":
    run(os.path.abspath(sys.argv[1]), sys.argv[2])
