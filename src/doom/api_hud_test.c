// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Martin Schröder <info@swedishembedded.com>
//
// What the agent overlay has to be true of before it is allowed near the
// video path.
//
// The overlay is the only part of the engine an agent can make draw arbitrary
// text, so its store is the place a malformed push turns into a buffer
// overrun and its wrapper is the place a long word turns into a hang. These
// checks pin the store and the line breaker, which is where those two live;
// they need no WAD, no display and no engine.
//
// Swedish Embedded AB builds observable control loops for its clients. If
// your team needs expertise in instrumenting a simulator so a policy's inputs
// and outputs are visible while it runs, you can procure our services by
// sending an email to info@swedishembedded.com.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_hud.h"
#include "hu_stuff.h"

// ------------------------------------------------------------ engine stubs
//
// The store and the line breaker are pure. Everything below is only what the
// drawer refers to, so that api_hud.c links without the video code.

patch_t *hu_font[HU_FONTSIZE];
int viewwindowx;
int viewwindowy;
int viewheight;
int scaledviewwidth;

void V_DrawPatchDirect(int x, int y, patch_t *patch)
{
    (void) x; (void) y; (void) patch;
}

void V_DrawFilledBox(int x, int y, int w, int h, int c)
{
    (void) x; (void) y; (void) w; (void) h; (void) c;
}

int I_GetPaletteIndex(int r, int g, int b)
{
    (void) r; (void) g; (void) b;
    return 0;
}

api_response_t API_CreateErrorResponse(int status, char *message)
{
    (void) message;
    return (api_response_t) { status, NULL };
}

// ----------------------------------------------------------------- harness

static int failures;
static int checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond)
    {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

static api_response_t post(const char *body)
{
    cJSON *json = cJSON_Parse(body);
    api_response_t resp = API_PostHud(json);

    if (json != NULL)
    {
        cJSON_Delete(json);
    }
    return resp;
}

// ------------------------------------------------------------------- tests

// Nothing pushed means nothing to draw. The drawer leans on this: it is the
// only reason it can be called unconditionally from the frame loop.
static void test_starts_empty(void)
{
    printf("inert until pushed\n");
    check(API_Hud_Count() == 0, "no lines before any push");
}

// The three things the agent shows: the observation it read, the options it
// weighed, and the one it took.
static void test_push_replaces_overlay(void)
{
    api_response_t resp;

    printf("a push replaces the whole overlay\n");

    resp = post("{\"lines\":["
                "{\"text\":\"health 100 armor 0\"},"
                "{\"text\":\"0.71 attack\",\"highlight\":true},"
                "{\"text\":\"0.29 turn_left\",\"highlight\":false}]}");

    check(resp.status_code == 204, "push accepted");
    check(API_Hud_Count() == 3, "three lines stored");
    check(strcmp(API_Hud_Text(0), "health 100 armor 0") == 0, "first line text");
    check(strcmp(API_Hud_Text(1), "0.71 attack") == 0, "second line text");
    check(!API_Hud_Highlight(0), "unmarked line is not highlighted");
    check(API_Hud_Highlight(1), "marked line is highlighted");
    check(!API_Hud_Highlight(2), "explicit false is not highlighted");

    resp = post("{\"lines\":[{\"text\":\"only one\"}]}");
    check(resp.status_code == 204, "second push accepted");
    check(API_Hud_Count() == 1, "push replaces rather than appends");
}

static void test_clear(void)
{
    api_response_t resp;

    printf("clearing\n");

    post("{\"lines\":[{\"text\":\"something\"}]}");
    resp = post("{\"lines\":[]}");
    check(resp.status_code == 204, "empty array accepted");
    check(API_Hud_Count() == 0, "empty array draws nothing");

    post("{\"lines\":[{\"text\":\"something\"}]}");
    resp = API_DeleteHud();
    check(resp.status_code == 204, "delete accepted");
    check(API_Hud_Count() == 0, "delete draws nothing");
}

// A rejected push must not blank the overlay. An agent that gets a push wrong
// on one tic would otherwise lose the frame it was in the middle of
// explaining, which is the frame someone is watching.
static void test_bad_push_is_rejected_and_harmless(void)
{
    const char *bad[] = {
        "",                                     // unparseable
        "{}",                                    // no lines
        "{\"lines\":\"nope\"}",                  // lines not an array
        "{\"lines\":[{\"highlight\":true}]}",    // element without text
        "{\"lines\":[{\"text\":7}]}",            // text not a string
        "{\"lines\":[[]]}",                      // element neither object nor string
        // Valid up to the point it is not: the case that tells an overlay
        // replaced in place apart from one replaced only once it is known good.
        "{\"lines\":[{\"text\":\"good\"},{\"text\":false}]}",
    };
    size_t i;

    printf("malformed pushes\n");

    post("{\"lines\":[{\"text\":\"keep me\"}]}");

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        api_response_t resp = post(bad[i]);

        check(resp.status_code == 400, bad[i][0] == '\0' ? "empty body rejected" : bad[i]);
    }

    check(API_Hud_Count() == 1 && strcmp(API_Hud_Text(0), "keep me") == 0,
       "overlay survives a rejected push");
}

// A bare string is shorthand for an unhighlighted line, because most of what
// an agent pushes has no decoration.
static void test_bare_string_element(void)
{
    printf("bare string shorthand\n");

    check(post("{\"lines\":[\"plain\"]}").status_code == 204, "bare string accepted");
    check(API_Hud_Count() == 1 && strcmp(API_Hud_Text(0), "plain") == 0, "stored verbatim");
    check(!API_Hud_Highlight(0), "bare string is not highlighted");
}

// Neither an over-long line nor an over-long list may reach the drawer, and
// neither is worth failing a training run over: both are clamped.
static void test_oversized_input_is_clamped(void)
{
    char body[8192];
    char huge[API_HUD_MAX_TEXT * 2];
    int i;
    size_t len;

    printf("oversized input\n");

    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    snprintf(body, sizeof(body), "{\"lines\":[{\"text\":\"%s\"}]}", huge);

    check(post(body).status_code == 204, "over-long text accepted");
    len = strlen(API_Hud_Text(0));
    check(len == API_HUD_MAX_TEXT - 1, "over-long text truncated to the store");

    len = snprintf(body, sizeof(body), "{\"lines\":[");
    for (i = 0; i < API_HUD_MAX_LINES * 2; i++)
    {
        len += snprintf(body + len, sizeof(body) - len, "%s{\"text\":\"l%d\"}",
                        i ? "," : "", i);
    }
    snprintf(body + len, sizeof(body) - len, "]}");

    check(post(body).status_code == 204, "over-long list accepted");
    check(API_Hud_Count() == API_HUD_MAX_LINES, "over-long list clamped");
    check(strcmp(API_Hud_Text(0), "l0") == 0, "the kept lines are the first ones");
}

// Control bytes would index the glyph table out of its range, and a tab or a
// newline has no glyph anyway.
static void test_text_is_sanitized(void)
{
    printf("sanitizing\n");

    post("{\"lines\":[{\"text\":\"a\\tb\\nc\"}]}");
    check(strcmp(API_Hud_Text(0), "a b c") == 0, "control bytes become spaces");
}

// Reading past the end is what a drawer does when it trusts a count it did
// not check.
static void test_out_of_range_reads_are_safe(void)
{
    printf("out of range reads\n");

    post("{\"lines\":[{\"text\":\"one\"}]}");
    check(API_Hud_Text(-1) == NULL, "negative index reads nothing");
    check(API_Hud_Text(1) == NULL, "index past the end reads nothing");
    check(API_Hud_Text(API_HUD_MAX_LINES + 99) == NULL, "far index reads nothing");
    check(!API_Hud_Highlight(1), "highlight past the end is false");
}

// The line breaker. A uniform glyph table makes the pixel budget countable in
// characters: 4px per glyph, 40px budget, so ten characters to a line.
static void test_wrapping(void)
{
    unsigned char width[256];
    const char *text;
    int n;
    int guard;

    printf("wrapping\n");

    memset(width, 4, sizeof(width));

    check(API_Hud_WrapPoint("short", width, 40) == 5, "a line that fits is not broken");

    n = API_Hud_WrapPoint("aaa bbb ccc ddd", width, 40);
    check(n == 7, "breaks at the last word boundary that fits");

    // A word with no break in it still has to make progress, and still may
    // not exceed the budget.
    n = API_Hud_WrapPoint("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", width, 40);
    check(n == 10, "an unbreakable word is broken at the budget");

    check(API_Hud_WrapPoint("", width, 40) == 0, "empty text yields no line");
    check(API_Hud_WrapPoint("aaaa", width, 2) == 1,
       "a glyph wider than the whole budget still advances");

    // Walking a string to its end must terminate and must cover every
    // character, or the drawer loops forever on a paragraph of observation.
    text = "the imp at 320 units bearing 41 degrees is the nearest threat";
    guard = 0;
    while (*text != '\0' && guard++ < 1000)
    {
        n = API_Hud_WrapPoint(text, width, 40);
        if (n <= 0)
        {
            break;
        }
        text += n;
        while (*text == ' ')
        {
            text++;
        }
    }
    check(*text == '\0', "wrapping a paragraph terminates at its end");
    check(guard < 1000, "wrapping always makes progress");
}

int main(void)
{
    test_starts_empty();
    test_push_replaces_overlay();
    test_clear();
    test_bad_push_is_rejected_and_harmless();
    test_bare_string_element();
    test_oversized_input_is_clamped();
    test_text_is_sanitized();
    test_out_of_range_reads_are_safe();
    test_wrapping();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
