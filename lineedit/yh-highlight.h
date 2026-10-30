#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

enum yh_style {
    YH_TEXT,
    YH_COMMAND,
    YH_KEYWORD,
    YH_STRING,
    YH_VARIABLE,
    YH_OPERATOR,
    YH_COMMENT,
    YH_ERROR
};

static int yh_name(wchar_t c)
{
    return c == L'_' || iswalnum(c);
}

static int yh_op(wchar_t c)
{
    return c && wcschr(L";&|()<>", c) != NULL;
}

static int yh_keyword(const wchar_t *s, size_t n)
{
    static const wchar_t *const words[] = {
        L"if", L"then", L"elif", L"else", L"fi", L"for", L"while",
        L"until", L"do", L"done", L"case", L"in", L"esac", L"!", NULL
    };

    for (size_t k = 0; words[k]; ++k)
        if (wcslen(words[k]) == n && !wmemcmp(s, words[k], n))
            return 1;

    return 0;
}

/*
 * No expansion, command execution, allocation or terminal I/O.
 * out needs n bytes.
 * n is typed input length, excluding ghost suggestions.
 *
 * This is a lexical approximation; heredocs and nested shell grammar are not
 * handled.
 */
static void yh_scan(const wchar_t *s, size_t n, unsigned char *out)
{
    size_t i = 0;
    int command = 1, redirect = 0;

    if (!n)
        return;

    memset(out, YH_TEXT, n);

    while (i < n) {
        if (iswspace(s[i])) {
            if (s[i] == L'\n')
                command = 1;
            ++i;
            continue;
        }

        if (s[i] == L'#') {
            while (i < n && s[i] != L'\n')
                out[i++] = YH_COMMENT;
            continue;
        }

        if (yh_op(s[i])) {
            wchar_t c = s[i];
            out[i++] = YH_OPERATOR;

            if (c == L'<' || c == L'>') {
                redirect = 1;
            } else {
                command = 1;
                redirect = 0;
            }
            continue;
        }

        size_t start = i;
        int quote = 0, plain = 1, assignment = 0;

        while (i < n) {
            wchar_t c = s[i];

            if (!quote && (iswspace(c) || yh_op(c)))
                break;

            if (c == L'\\' && quote != L'\'') {
                plain = 0;
                out[i++] = quote ? YH_STRING : YH_TEXT;
                if (i < n)
                    out[i++] = quote ? YH_STRING : YH_TEXT;
                continue;
            }

            if (c == L'\'' || c == L'"') {
                plain = 0;
                if (!quote)
                    quote = c;
                else if (quote == c)
                    quote = 0;
                out[i++] = YH_STRING;
                continue;
            }

            if (c == L'$' && quote != L'\'') {
                plain = 0;
                out[i++] = YH_VARIABLE;

                if (i < n && s[i] == L'{') {
                    out[i++] = YH_VARIABLE;
                    while (i < n && s[i] != L'}')
                        out[i++] = YH_VARIABLE;
                    if (i < n)
                        out[i++] = YH_VARIABLE;
                } else if (
                    i < n &&
                    wcschr(L"?$!#@*-0123456789", s[i])
                ) {
                    out[i++] = YH_VARIABLE;
                } else {
                    while (i < n && yh_name(s[i]))
                        out[i++] = YH_VARIABLE;
                }
                continue;
            }

            if (!quote && c == L'=' && i > start) {
                assignment = s[start] == L'_' || iswalpha(s[start]);
                for (size_t j = start; assignment && j < i; ++j)
                    if (!yh_name(s[j]))
                        assignment = 0;
            }

            out[i++] = quote ? YH_STRING : YH_TEXT;
        }

        if (plain && command && !redirect && !assignment) {
            int keyword = yh_keyword(s + start, i - start);
            memset(
                out + start,
                keyword ? YH_KEYWORD : YH_COMMAND,
                i - start
            );
            command = keyword;
        } else if (!redirect && !assignment) {
            command = 0;
        }

        redirect = 0;
    }
}
