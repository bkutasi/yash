# Real PTY tests for the cursor during incremental history search.

unset PYTHONOPTIMIZE

if ! command -v python3 >/dev/null 2>&1 ||
        ! testee -c 'command -bv bindkey && command -bv history' >/dev/null ||
        ! python3 -c 'import curses; curses.setupterm("xterm"); assert curses.tigetstr("kcub1") and curses.tigetstr("kcuf1")' >/dev/null 2>&1; then
    skip=true
fi

test_OE -e 0 'search match and accepted editing position' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" basic
__IN__

test_OE -e 0 'search left accepts and moves from match' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" left
__IN__

test_OE -e 0 'search right accepts and moves from match' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" right
__IN__

test_OE -e 0 'prediction allows incremental arrow navigation and End' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" prediction-navigation
__IN__

test_OE -e 0 'Tab completes a filename while editing' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" tab-completion
__IN__

test_OE -e 0 'repeated search updates the match cursor' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" repeat
__IN__

test_OE -e 0 'changing the search updates the match cursor' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" mutate
__IN__

test_OE -e 0 'search treats pattern metacharacters literally' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" literal
__IN__

test_OE -e 0 'search accounts for displayed control characters' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" control
__IN__

test_OE -e 0 'control character match starts after a terminal wrap' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" control-wrap
__IN__

test_OE -e 0 'accepted undo entry locates match in edited text' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" undo-entry
__IN__

test_OE -e 0 'dirty current line invalidates stale search snapshot' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" undo-dirty
__IN__

test_OE -e 0 'search does not accept a match removed by saved edits' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" undo-missing
__IN__

test_OE -e 0 'search does not accept a match split by saved edits' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" undo-split
__IN__

test_OE -e 0 'search match remains visible on a scrolling terminal' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" scroll
__IN__

test_OE -e 0 'late search match remains visible on a scrolling terminal' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" scroll-late
__IN__

test_OE -e 0 'long search query stays within the terminal height' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" scroll-long-query
__IN__

test_OE -e 0 'search recovers after a long draft scrolls the prompt away' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" scroll-draft
__IN__

test_OE -e 0 'scrolling search recovers after a failed query' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" scroll-no-match
__IN__

test_OE -e 0 'one-row terminal keeps the match visible' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" one-row
__IN__

test_OE -e 0 'compact search abort restores the prompt' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" prompt-restore
__IN__

test_OE -e 0 'long accepted search keeps editing cursor visible' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" long-accept
__IN__

test_OE -e 0 'narrow search viewport retains query text' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" narrow-query
__IN__

test_OE -e 0 'left after unmatched search preserves the original line' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" no-match-left
__IN__

test_OE -e 0 'right after unmatched search preserves the original line' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" no-match-right
__IN__

test_OE -e 0 'match starts at an exact terminal wrap' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" exact-wrap
__IN__

test_OE -e 0 'accepting empty search preserves the original line' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" empty
__IN__

test_OE -e 0 'repeated search can recover after clearing the query' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" repeat-empty
__IN__

test_OE -e 0 'accepting cleared search preserves its history anchor' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" repeat-empty-accept
__IN__

if ! python3 -c 'import locale; locale.setlocale(locale.LC_CTYPE, "C.UTF-8")' \
        >/dev/null 2>&1; then
    skip=true
fi

test_OE -e 0 'search cursor counts Unicode display cells' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" unicode
__IN__

test_OE -e 0 'wide match wraps at terminal edge' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" wide-wrap
__IN__

test_OE -e 0 'combining mark match follows its displayed representation' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" combining
__IN__

test_OE -e 0 'search skips history invalid in the current locale' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" invalid-history
__IN__

test_OE -e 0 'locale change invalidates an active search result' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" invalid-redraw
__IN__

test_OE -e 0 'enter rejects a search result invalidated by locale change' \
    -c 'exec python3 "$@"' yash "${abs_test_file%/*}/lineedit-search-pty.py" "$testee" invalid-enter
__IN__
