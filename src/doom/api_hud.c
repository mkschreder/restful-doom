// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Martin Schröder <info@swedishembedded.com>
//
// See api_hud.h for the wire format and for why the overlay exists.

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "api_hud.h"

#include "doomdef.h"
#include "hu_stuff.h"
#include "i_swap.h"
#include "i_video.h"
#include "r_main.h"
#include "r_state.h"
#include "v_video.h"

typedef struct
{
    char text[API_HUD_MAX_TEXT];
    boolean highlight;
} api_hud_line_t;

static api_hud_line_t hud_lines[API_HUD_MAX_LINES];
static int line_count;

// ------------------------------------------------------------------- store

// Copy into a fixed line, truncating rather than failing, and flatten
// anything that is not a printable ASCII glyph to a space. The flattening is
// not cosmetic: the drawer indexes a glyph table with these bytes, and the
// engine font has an entry for nothing outside '!'..'_'.
static void StoreText(char *dest, const char *src)
{
    int i;

    for (i = 0; i < API_HUD_MAX_TEXT - 1 && src[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char) src[i];

        dest[i] = (c < 0x20 || c > 0x7e) ? ' ' : (char) c;
    }
    dest[i] = '\0';
}

void API_Hud_Clear(void)
{
    line_count = 0;
}

int API_Hud_Count(void)
{
    return line_count;
}

const char *API_Hud_Text(int index)
{
    if (index < 0 || index >= line_count)
    {
        return NULL;
    }
    return hud_lines[index].text;
}

boolean API_Hud_Highlight(int index)
{
    if (index < 0 || index >= line_count)
    {
        return false;
    }
    return hud_lines[index].highlight;
}

// ---------------------------------------------------------------- handlers

// Pull one array element apart, or say why it is not a line.
static const char *ReadLine(cJSON *element, const char **text,
                            boolean *highlight)
{
    cJSON *text_item;
    cJSON *highlight_item;

    if (cJSON_IsString(element))
    {
        *text = element->valuestring;
        *highlight = false;
        return NULL;
    }

    if (!cJSON_IsObject(element))
    {
        return "each line must be an object or a string";
    }

    text_item = cJSON_GetObjectItem(element, "text");
    if (!cJSON_IsString(text_item))
    {
        return "each line needs a string text";
    }

    highlight_item = cJSON_GetObjectItem(element, "highlight");
    if (highlight_item != NULL && !cJSON_IsBool(highlight_item))
    {
        return "highlight must be a boolean";
    }

    *text = text_item->valuestring;
    *highlight = highlight_item != NULL && cJSON_IsTrue(highlight_item);
    return NULL;
}

api_response_t API_PostHud(cJSON *req)
{
    cJSON *array;
    cJSON *element;
    int count = 0;

    array = cJSON_GetObjectItem(req, "lines");
    if (!cJSON_IsArray(array))
    {
        return API_CreateErrorResponse(400, "lines must be an array");
    }

    // Validated in full before anything is stored. A push that turns out to
    // be malformed at its last element must leave the overlay exactly as it
    // was, not half replaced: the frame on screen is the one the agent is
    // part way through explaining.
    cJSON_ArrayForEach(element, array)
    {
        const char *text;
        boolean highlight;
        const char *problem = ReadLine(element, &text, &highlight);

        if (problem != NULL)
        {
            return API_CreateErrorResponse(400, (char *) problem);
        }
    }

    cJSON_ArrayForEach(element, array)
    {
        const char *text;
        boolean highlight;

        if (count >= API_HUD_MAX_LINES)
        {
            break;
        }

        ReadLine(element, &text, &highlight);
        StoreText(hud_lines[count].text, text);
        hud_lines[count].highlight = highlight;
        count++;
    }

    line_count = count;

    return (api_response_t) { 204, NULL };
}

api_response_t API_DeleteHud(void)
{
    API_Hud_Clear();
    return (api_response_t) { 204, NULL };
}

// ------------------------------------------------------------------ layout

int API_Hud_WrapPoint(const char *text, const unsigned char *glyph_width,
                      int budget)
{
    int width = 0;
    int i = 0;
    int last_space = 0;

    if (text == NULL || glyph_width == NULL)
    {
        return 0;
    }

    while (text[i] != '\0')
    {
        int glyph = glyph_width[(unsigned char) text[i]];

        // The first glyph is placed unconditionally. Without that a budget
        // narrower than a single character returns zero, and a caller walking
        // the string never reaches its end.
        if (i > 0 && width + glyph > budget)
        {
            break;
        }
        if (text[i] == ' ')
        {
            last_space = i;
        }
        width += glyph;
        i++;
    }

    if (text[i] == '\0')
    {
        return i;
    }
    if (last_space > 0)
    {
        return last_space;
    }
    return i;
}

// ------------------------------------------------------------------ drawer

// Pixel width of every byte the store can hold, built from the engine font.
// Rebuilt when the font changes identity, which happens once, at load.
static unsigned char glyph_width[256];
static const patch_t *glyph_width_font;

static void BuildGlyphWidths(void)
{
    int c;

    for (c = 0; c < 256; c++)
    {
        int index = toupper(c) - HU_FONTSTART;

        // Same fallback as the menu's own text writer: a byte the font has no
        // glyph for still advances, so a space stays a space.
        if (index < 0 || index >= HU_FONTSIZE || hu_font[index] == NULL)
        {
            glyph_width[c] = 4;
        }
        else
        {
            glyph_width[c] = (unsigned char) SHORT(hu_font[index]->width);
        }
    }
    glyph_width_font = hu_font[0];
}

// V_DrawFilledBox does no clipping of its own, and V_DrawPatch only range
// checks under a build option this build does not set. So both are fenced
// here rather than trusted.
static void FillClipped(int x, int y, int w, int h, int colour)
{
    if (x < 0)
    {
        w += x;
        x = 0;
    }
    if (y < 0)
    {
        h += y;
        y = 0;
    }
    if (x + w > SCREENWIDTH)
    {
        w = SCREENWIDTH - x;
    }
    if (y + h > SCREENHEIGHT)
    {
        h = SCREENHEIGHT - y;
    }
    if (w > 0 && h > 0)
    {
        V_DrawFilledBox(x, y, w, h, colour);
    }
}

static void DrawGlyphIfOnScreen(int x, int y, patch_t *patch)
{
    int px = x - SHORT(patch->leftoffset);
    int py = y - SHORT(patch->topoffset);

    if (px < 0 || px + SHORT(patch->width) > SCREENWIDTH
     || py < 0 || py + SHORT(patch->height) > SCREENHEIGHT)
    {
        return;
    }
    V_DrawPatchDirect(x, y, patch);
}

// How wide a row will actually be drawn.
//
// The band behind a line is sized to its text rather than to the view. A full
// width band per line turns a dozen lines into a panel over the whole screen,
// and then the overlay is the only thing anybody can see - which defeats the
// point of drawing it on the game rather than beside it.
static int RowWidth(const char *text, int length)
{
    int i, w = 0;

    for (i = 0; i < length; i++)
    {
        int index = toupper((unsigned char) text[i]) - HU_FONTSTART;

        w += (index < 0 || index >= HU_FONTSIZE || hu_font[index] == NULL)
                 ? 4 : SHORT(hu_font[index]->width);
    }
    return w;
}

// Draw one wrapped row, left aligned in the band, and stop at its right edge.
static void DrawRow(const char *text, int length, int x, int y, int right)
{
    int i;

    for (i = 0; i < length; i++)
    {
        int index = toupper((unsigned char) text[i]) - HU_FONTSTART;
        int width;

        if (index < 0 || index >= HU_FONTSIZE || hu_font[index] == NULL)
        {
            x += 4;
            continue;
        }

        width = SHORT(hu_font[index]->width);
        if (x + width > right)
        {
            break;
        }
        DrawGlyphIfOnScreen(x, y, hu_font[index]);
        x += width;
    }
}

void API_Hud_Drawer(void)
{
    int colour_plain;
    int colour_marked;
    int step;
    int left;
    int right;
    int top;
    int bottom;
    int y;
    int i;

    // Inert until an agent pushes something. This is what lets the frame loop
    // call the overlay unconditionally.
    if (line_count == 0 || hu_font[0] == NULL)
    {
        return;
    }

    // Confined to the rectangle the renderer repaints every frame. Outside
    // it - the status bar, the border around a reduced view - the engine only
    // repaints on demand, and text drawn there would smear across the frames
    // that follow instead of being erased with the view.
    left = viewwindowx;
    top = viewwindowy;
    right = viewwindowx + scaledviewwidth;
    bottom = viewwindowy + viewheight;

    if (left < 0)
    {
        left = 0;
    }
    if (top < 0)
    {
        top = 0;
    }
    if (right > SCREENWIDTH)
    {
        right = SCREENWIDTH;
    }
    if (bottom > SCREENHEIGHT)
    {
        bottom = SCREENHEIGHT;
    }
    if (right - left < 8 || bottom - top < 8)
    {
        return;
    }

    if (glyph_width_font != hu_font[0])
    {
        BuildGlyphWidths();
    }

    step = SHORT(hu_font[0]->height) + 2;

    // Asked for by colour rather than by index so the overlay reads the same
    // against whatever palette the loaded IWAD supplies.
    colour_plain = I_GetPaletteIndex(0x00, 0x00, 0x00);
    colour_marked = I_GetPaletteIndex(0xa0, 0x00, 0x00);

    y = top;

    for (i = 0; i < line_count && y + step <= bottom; i++)
    {
        const char *text = hud_lines[i].text;
        int colour = hud_lines[i].highlight ? colour_marked : colour_plain;

        // An empty line is a deliberate separator, so it still gets a band.
        do
        {
            int length = API_Hud_WrapPoint(text, glyph_width, right - left - 4);

            {
                int w = RowWidth(text, length) + 4;

                if (w > right - left)
                {
                    w = right - left;
                }
                FillClipped(left, y, w, step, colour);
            }
            DrawRow(text, length, left + 2, y + 1, right - 2);
            y += step;

            text += length;
            while (*text == ' ')
            {
                text++;
            }
        }
        while (*text != '\0' && y + step <= bottom);
    }
}
