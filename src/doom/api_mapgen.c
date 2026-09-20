//
// Building a DOOM level in memory, including its BSP. See api_mapgen.h.
//
// The node builder is the naive one: pick a wall, split the rest of the walls
// against the line it lies on, recurse. It is quadratic in the number of
// walls per subtree and that is fine, because the maps it builds are rooms
// and corridors with a few hundred walls, compiled once per episode.
//
// It is exact on axis-aligned geometry. A partition that crosses a wall at an
// angle can land between two whole map units, and DOOM vertices are whole map
// units, so such a split is rounded and counted - see MapGen_InexactSplits.
//

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "api_mapgen.h"
#include "doomdata.h"
#include "m_bbox.h"

typedef struct
{
    int x, y;
} gvert_t;

typedef struct
{
    int v1, v2;
    int flags, special, tag;
    int side[2];
} gline_t;

typedef struct
{
    char top[8], bottom[8], mid[8];
    int sector;
} gside_t;

typedef struct
{
    int floorheight, ceilingheight;
    char floorpic[8], ceilingpic[8];
    int light, special, tag;
} gsector_t;

typedef struct
{
    int x, y, angle, type, options;
} gthing_t;

struct mapgen_s
{
    gvert_t *verts;     int nverts, cverts;
    gline_t *lines;     int nlines, clines;
    gside_t *sides;     int nsides, csides;
    gsector_t *sectors; int nsectors, csectors;
    gthing_t *things;   int nthings, cthings;
    int inexact;
};

// A wall, or a piece of one, as the BSP hands it around.
typedef struct
{
    int v1, v2;
    int line;
    int side;
    int offset;
} bseg_t;

typedef struct
{
    mapgen_t *m;
    bseg_t *segs;           int nsegs, csegs;
    mapsubsector_t *ss;     int nss, css;
    mapnode_t *nodes;       int nnodes, cnodes;
    int inexact;
    int failed;
} build_t;

// ------------------------------------------------------------------ arrays

static void *grow(void *p, int want, int *cap, size_t elem)
{
    int c = *cap;

    if (want < c)
    {
        return p;
    }
    if (c == 0)
    {
        c = 16;
    }
    while (want >= c)
    {
        c *= 2;
    }
    *cap = c;
    return realloc(p, (size_t)c * elem);
}

static void name8(char *dst, const char *src)
{
    memset(dst, 0, 8);
    if (src == NULL)
    {
        dst[0] = '-';
        return;
    }
    strncpy(dst, src, 8);
}

// ------------------------------------------------------------------- build

mapgen_t *MapGen_New(void)
{
    mapgen_t *m = calloc(1, sizeof(mapgen_t));

    return m;
}

void MapGen_Free(mapgen_t *m)
{
    if (m == NULL)
    {
        return;
    }
    free(m->verts);
    free(m->lines);
    free(m->sides);
    free(m->sectors);
    free(m->things);
    free(m);
}

int MapGen_NumSectors(const mapgen_t *m) { return m->nsectors; }
int MapGen_NumLines(const mapgen_t *m) { return m->nlines; }
int MapGen_NumThings(const mapgen_t *m) { return m->nthings; }
int MapGen_InexactSplits(const mapgen_t *m) { return m->inexact; }

static int vertex(mapgen_t *m, int x, int y)
{
    int i;

    for (i = 0; i < m->nverts; i++)
    {
        if (m->verts[i].x == x && m->verts[i].y == y)
        {
            return i;
        }
    }
    m->verts = grow(m->verts, m->nverts, &m->cverts, sizeof(gvert_t));
    m->verts[m->nverts].x = x;
    m->verts[m->nverts].y = y;
    return m->nverts++;
}

int MapGen_AddSector(mapgen_t *m, int floorheight, int ceilingheight,
                     const char *floorpic, const char *ceilingpic,
                     int light, int special, int tag)
{
    gsector_t *s;

    m->sectors = grow(m->sectors, m->nsectors, &m->csectors, sizeof(gsector_t));
    s = &m->sectors[m->nsectors];
    s->floorheight = floorheight;
    s->ceilingheight = ceilingheight;
    name8(s->floorpic, floorpic);
    name8(s->ceilingpic, ceilingpic);
    s->light = light;
    s->special = special;
    s->tag = tag;
    return m->nsectors++;
}

static int sidedef(mapgen_t *m, int sector, const char *top,
                   const char *bottom, const char *mid)
{
    gside_t *s;

    m->sides = grow(m->sides, m->nsides, &m->csides, sizeof(gside_t));
    s = &m->sides[m->nsides];
    name8(s->top, top);
    name8(s->bottom, bottom);
    name8(s->mid, mid);
    s->sector = sector;
    return m->nsides++;
}

static int addline(mapgen_t *m, int x1, int y1, int x2, int y2,
                   int front, int back, int flags)
{
    gline_t *l;

    m->lines = grow(m->lines, m->nlines, &m->clines, sizeof(gline_t));
    l = &m->lines[m->nlines];
    l->v1 = vertex(m, x1, y1);
    l->v2 = vertex(m, x2, y2);
    l->flags = flags;
    l->special = 0;
    l->tag = 0;
    l->side[0] = front;
    l->side[1] = back;
    return m->nlines++;
}

int MapGen_AddWall(mapgen_t *m, int x1, int y1, int x2, int y2,
                   int sector, const char *texture)
{
    int front = sidedef(m, sector, NULL, NULL, texture);

    return addline(m, x1, y1, x2, y2, front, -1, ML_BLOCKING);
}

int MapGen_AddPortal(mapgen_t *m, int x1, int y1, int x2, int y2,
                     int frontsector, int backsector,
                     const char *uppertexture, const char *lowertexture,
                     const char *midtexture)
{
    int front = sidedef(m, frontsector, uppertexture, lowertexture, midtexture);
    int back = sidedef(m, backsector, uppertexture, lowertexture, midtexture);

    return addline(m, x1, y1, x2, y2, front, back, ML_TWOSIDED);
}

void MapGen_SetLineFlags(mapgen_t *m, int flags)
{
    if (m->nlines > 0)
    {
        m->lines[m->nlines - 1].flags = flags;
    }
}

void MapGen_SetLineSpecial(mapgen_t *m, int special, int tag)
{
    if (m->nlines > 0)
    {
        m->lines[m->nlines - 1].special = special;
        m->lines[m->nlines - 1].tag = tag;
    }
}

void MapGen_AddRoom(mapgen_t *m, int x1, int y1, int x2, int y2,
                    int sector, const char *texture)
{
    MapGen_AddWall(m, x1, y1, x1, y2, sector, texture);
    MapGen_AddWall(m, x1, y2, x2, y2, sector, texture);
    MapGen_AddWall(m, x2, y2, x2, y1, sector, texture);
    MapGen_AddWall(m, x2, y1, x1, y1, sector, texture);
}

void MapGen_AddThing(mapgen_t *m, int x, int y, int angle, int type,
                     int options)
{
    gthing_t *t;

    m->things = grow(m->things, m->nthings, &m->cthings, sizeof(gthing_t));
    t = &m->things[m->nthings];
    t->x = x;
    t->y = y;
    t->angle = angle;
    t->type = type;
    t->options = options;
    m->nthings++;
}

// --------------------------------------------------------------- the BSP

#define SIDE_FRONT 0
#define SIDE_BACK 1
#define SIDE_SPLIT 2

static long long cross_of(const build_t *b, const bseg_t *part, int x, int y)
{
    const gvert_t *a = &b->m->verts[part->v1];
    const gvert_t *c = &b->m->verts[part->v2];
    long long pdx = c->x - a->x;
    long long pdy = c->y - a->y;

    return pdy * (x - a->x) - pdx * (y - a->y);
}

static int classify(const build_t *b, const bseg_t *part, const bseg_t *s)
{
    long long ca = cross_of(b, part, b->m->verts[s->v1].x, b->m->verts[s->v1].y);
    long long cb = cross_of(b, part, b->m->verts[s->v2].x, b->m->verts[s->v2].y);

    if (ca == 0 && cb == 0)
    {
        // Lying on the partition. It goes with whichever way it faces, so
        // that a wall and the partition drawn along it end up together.
        const gvert_t *pa = &b->m->verts[part->v1];
        const gvert_t *pc = &b->m->verts[part->v2];
        long long dot = (long long)(b->m->verts[s->v2].x - b->m->verts[s->v1].x) * (pc->x - pa->x)
                      + (long long)(b->m->verts[s->v2].y - b->m->verts[s->v1].y) * (pc->y - pa->y);
        return dot >= 0 ? SIDE_FRONT : SIDE_BACK;
    }
    if (ca >= 0 && cb >= 0)
    {
        return SIDE_FRONT;
    }
    if (ca <= 0 && cb <= 0)
    {
        return SIDE_BACK;
    }
    return SIDE_SPLIT;
}

// Where `part`'s line crosses the segment from A to B. Rounds to whole map
// units and says whether it had to.
static int intersect(build_t *b, const bseg_t *part, int ax, int ay,
                     int bx, int by, int *ix, int *iy)
{
    const gvert_t *pa = &b->m->verts[part->v1];
    const gvert_t *pc = &b->m->verts[part->v2];
    long long pdx = pc->x - pa->x;
    long long pdy = pc->y - pa->y;
    long long dx = bx - ax;
    long long dy = by - ay;
    long long ca = pdy * (ax - pa->x) - pdx * (ay - pa->y);
    long long den = pdy * dx - pdx * dy;
    long long nx, ny;
    int exact;

    if (den == 0)
    {
        *ix = ax;
        *iy = ay;
        return 0;
    }
    nx = ca * dx;
    ny = ca * dy;
    exact = (nx % den == 0) && (ny % den == 0);
    *ix = (int)(ax - (long long)llround((double)nx / (double)den));
    *iy = (int)(ay - (long long)llround((double)ny / (double)den));
    return exact;
}

static int seg_len(const build_t *b, int v1, int v2)
{
    double dx = b->m->verts[v2].x - b->m->verts[v1].x;
    double dy = b->m->verts[v2].y - b->m->verts[v1].y;

    return (int)(sqrt(dx * dx + dy * dy) + 0.5);
}

// Chooses the wall to cut on. NULL when nothing is behind anything, which is
// what convex means.
static int choose_partition(const build_t *b, const bseg_t *list, int n)
{
    int i, j, best = -1;
    long long bestcost = 0;

    for (i = 0; i < n; i++)
    {
        int front = 0, back = 0, splits = 0;
        long long cost;

        for (j = 0; j < n; j++)
        {
            int side;

            if (i == j)
            {
                continue;
            }
            side = classify(b, &list[i], &list[j]);
            if (side == SIDE_FRONT) front++;
            else if (side == SIDE_BACK) back++;
            else { splits++; front++; back++; }
        }
        if (back == 0)
        {
            continue;
        }
        cost = (long long)splits * 16 + llabs(front - back);
        if (best == -1 || cost < bestcost)
        {
            best = i;
            bestcost = cost;
        }
    }
    return best;
}

static void bbox_of(const build_t *b, const bseg_t *list, int n, short *bbox)
{
    int i;
    int top = -32768, bottom = 32767, left = 32767, right = -32768;

    for (i = 0; i < n; i++)
    {
        int k;

        for (k = 0; k < 2; k++)
        {
            const gvert_t *v = &b->m->verts[k == 0 ? list[i].v1 : list[i].v2];

            if (v->y > top) top = v->y;
            if (v->y < bottom) bottom = v->y;
            if (v->x < left) left = v->x;
            if (v->x > right) right = v->x;
        }
    }
    bbox[BOXTOP] = (short)top;
    bbox[BOXBOTTOM] = (short)bottom;
    bbox[BOXLEFT] = (short)left;
    bbox[BOXRIGHT] = (short)right;
}

static unsigned short make_subsector(build_t *b, const bseg_t *list, int n)
{
    int first = b->nsegs;
    int i;

    for (i = 0; i < n; i++)
    {
        b->segs = grow(b->segs, b->nsegs, &b->csegs, sizeof(bseg_t));
        b->segs[b->nsegs++] = list[i];
    }
    b->ss = grow(b->ss, b->nss, &b->css, sizeof(mapsubsector_t));
    b->ss[b->nss].numsegs = (short)n;
    b->ss[b->nss].firstseg = (short)first;
    return (unsigned short)(NF_SUBSECTOR | b->nss++);
}

static unsigned short divide(build_t *b, bseg_t *list, int n);

static unsigned short divide(build_t *b, bseg_t *list, int n)
{
    int pick, i;
    bseg_t part;
    bseg_t *front, *back;
    int nf = 0, nb = 0;
    mapnode_t node;
    unsigned short kids[2];
    int index;
    const gvert_t *pa, *pc;

    if (b->failed)
    {
        return (unsigned short)NF_SUBSECTOR;
    }
    pick = choose_partition(b, list, n);
    if (pick < 0)
    {
        return make_subsector(b, list, n);
    }
    part = list[pick];

    front = malloc(sizeof(bseg_t) * (size_t)(n + 1) * 2);
    back = malloc(sizeof(bseg_t) * (size_t)(n + 1) * 2);
    if (front == NULL || back == NULL)
    {
        free(front);
        free(back);
        b->failed = 1;
        return (unsigned short)NF_SUBSECTOR;
    }

    for (i = 0; i < n; i++)
    {
        int side = classify(b, &part, &list[i]);

        if (side == SIDE_FRONT)
        {
            front[nf++] = list[i];
        }
        else if (side == SIDE_BACK)
        {
            back[nb++] = list[i];
        }
        else
        {
            bseg_t head = list[i], tail = list[i];
            int ix, iy, mid;
            long long ca;

            if (!intersect(b, &part,
                           b->m->verts[list[i].v1].x, b->m->verts[list[i].v1].y,
                           b->m->verts[list[i].v2].x, b->m->verts[list[i].v2].y,
                           &ix, &iy))
            {
                b->inexact++;
            }
            mid = vertex(b->m, ix, iy);
            head.v2 = mid;
            tail.v1 = mid;
            tail.offset = list[i].offset + seg_len(b, list[i].v1, mid);

            ca = cross_of(b, &part, b->m->verts[list[i].v1].x,
                          b->m->verts[list[i].v1].y);
            if (ca > 0)
            {
                front[nf++] = head;
                back[nb++] = tail;
            }
            else
            {
                back[nb++] = head;
                front[nf++] = tail;
            }
        }
    }

    // The partition itself is collinear with its own line, so classify() put
    // it in front. Both sides must have shrunk or this would not terminate.
    if (nf == 0 || nb == 0)
    {
        free(front);
        free(back);
        return make_subsector(b, list, n);
    }

    memset(&node, 0, sizeof(node));
    pa = &b->m->verts[part.v1];
    pc = &b->m->verts[part.v2];
    node.x = (short)pa->x;
    node.y = (short)pa->y;
    node.dx = (short)(pc->x - pa->x);
    node.dy = (short)(pc->y - pa->y);
    {
        short box[4];

        bbox_of(b, front, nf, box);
        memcpy(node.bbox[0], box, sizeof(box));
        bbox_of(b, back, nb, box);
        memcpy(node.bbox[1], box, sizeof(box));
    }

    kids[0] = divide(b, front, nf);
    kids[1] = divide(b, back, nb);
    free(front);
    free(back);

    node.children[0] = kids[0];
    node.children[1] = kids[1];

    b->nodes = grow(b->nodes, b->nnodes, &b->cnodes, sizeof(mapnode_t));
    index = b->nnodes;
    b->nodes[b->nnodes++] = node;
    return (unsigned short)index;
}

// --------------------------------------------------------------- blockmap

static int line_hits_block(const mapgen_t *m, const gline_t *l,
                           int x0, int y0, int x1, int y1)
{
    int ax = m->verts[l->v1].x, ay = m->verts[l->v1].y;
    int bx = m->verts[l->v2].x, by = m->verts[l->v2].y;
    int lo, hi;
    long long c[4];
    int i, pos = 0, neg = 0;

    lo = ax < bx ? ax : bx;
    hi = ax < bx ? bx : ax;
    if (hi < x0 || lo > x1)
    {
        return 0;
    }
    lo = ay < by ? ay : by;
    hi = ay < by ? by : ay;
    if (hi < y0 || lo > y1)
    {
        return 0;
    }
    if (ax == bx || ay == by)
    {
        return 1;
    }
    // A diagonal crosses the block only if the block's corners fall on both
    // sides of it.
    c[0] = (long long)(by - ay) * (x0 - ax) - (long long)(bx - ax) * (y0 - ay);
    c[1] = (long long)(by - ay) * (x1 - ax) - (long long)(bx - ax) * (y0 - ay);
    c[2] = (long long)(by - ay) * (x0 - ax) - (long long)(bx - ax) * (y1 - ay);
    c[3] = (long long)(by - ay) * (x1 - ax) - (long long)(bx - ax) * (y1 - ay);
    for (i = 0; i < 4; i++)
    {
        if (c[i] > 0) pos++;
        else if (c[i] < 0) neg++;
        else { pos++; neg++; }
    }
    return pos > 0 && neg > 0;
}

static short *make_blockmap(const mapgen_t *m, int *count, const char **why)
{
    int minx = 32767, miny = 32767, maxx = -32768, maxy = -32768;
    int originx, originy, cols, rows, bx, by, i;
    short *out = NULL;
    int n = 0, cap = 0;

    for (i = 0; i < m->nverts; i++)
    {
        if (m->verts[i].x < minx) minx = m->verts[i].x;
        if (m->verts[i].y < miny) miny = m->verts[i].y;
        if (m->verts[i].x > maxx) maxx = m->verts[i].x;
        if (m->verts[i].y > maxy) maxy = m->verts[i].y;
    }
    if (m->nverts == 0)
    {
        *why = "a map with no vertices";
        return NULL;
    }
    originx = minx - 8;
    originy = miny - 8;
    cols = (maxx - originx) / 128 + 1;
    rows = (maxy - originy) / 128 + 1;

    out = grow(out, 4 + cols * rows, &cap, sizeof(short));
    out[0] = (short)originx;
    out[1] = (short)originy;
    out[2] = (short)cols;
    out[3] = (short)rows;
    n = 4 + cols * rows;

    for (by = 0; by < rows; by++)
    {
        for (bx = 0; bx < cols; bx++)
        {
            int x0 = originx + bx * 128, y0 = originy + by * 128;

            if (n > 32767)
            {
                free(out);
                *why = "the blockmap outgrew what a DOOM level can hold";
                return NULL;
            }
            out[4 + by * cols + bx] = (short)n;
            // Vanilla blocklists open with a zero the engine reads as
            // linedef 0 and then rejects on its own geometry.
            out = grow(out, n, &cap, sizeof(short));
            out[n++] = 0;
            for (i = 0; i < m->nlines; i++)
            {
                if (line_hits_block(m, &m->lines[i], x0, y0, x0 + 128, y0 + 128))
                {
                    out = grow(out, n, &cap, sizeof(short));
                    out[n++] = (short)i;
                }
            }
            out = grow(out, n, &cap, sizeof(short));
            out[n++] = -1;
        }
    }
    *count = n;
    return out;
}

// ------------------------------------------------------------------ output

typedef struct
{
    byte *data;
    size_t len, cap;
} blob_t;

static void put(blob_t *b, const void *p, size_t n)
{
    if (b->len + n > b->cap)
    {
        b->cap = (b->cap ? b->cap : 1024);
        while (b->len + n > b->cap)
        {
            b->cap *= 2;
        }
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static short bam16(int dx, int dy)
{
    double a = atan2((double)dy, (double)dx);
    double turns = a / (2.0 * 3.14159265358979323846);
    long long bam;

    if (turns < 0)
    {
        turns += 1.0;
    }
    bam = (long long)(turns * 4294967296.0 + 0.5) & 0xffffffffLL;
    return (short)(unsigned short)((bam >> 16) & 0xffff);
}

byte *MapGen_Build(mapgen_t *m, const char *lumpname, size_t *len,
                   const char **why)
{
    build_t b;
    bseg_t *list = NULL;
    int nlist = 0, clist = 0;
    int i;
    short *blockmap = NULL;
    int blockmapcount = 0;
    blob_t out;
    int filepos[11], size[11];
    int infotableofs, numlumps = 11;
    byte *dir;
    const char *local_why = "";

    if (why == NULL)
    {
        why = &local_why;
    }
    *why = NULL;
    memset(&b, 0, sizeof(b));
    b.m = m;
    m->inexact = 0;

    if (m->nlines == 0 || m->nsectors == 0)
    {
        *why = "a map needs at least one sector and one wall";
        return NULL;
    }

    for (i = 0; i < m->nlines; i++)
    {
        bseg_t s;

        s.line = i;
        s.side = 0;
        s.v1 = m->lines[i].v1;
        s.v2 = m->lines[i].v2;
        s.offset = 0;
        list = grow(list, nlist, &clist, sizeof(bseg_t));
        list[nlist++] = s;

        if (m->lines[i].side[1] >= 0)
        {
            s.side = 1;
            s.v1 = m->lines[i].v2;
            s.v2 = m->lines[i].v1;
            s.offset = 0;
            list = grow(list, nlist, &clist, sizeof(bseg_t));
            list[nlist++] = s;
        }
    }

    divide(&b, list, nlist);
    free(list);
    m->inexact = b.inexact;

    if (b.failed)
    {
        free(b.segs);
        free(b.ss);
        free(b.nodes);
        *why = "ran out of memory building the BSP";
        return NULL;
    }

    blockmap = make_blockmap(m, &blockmapcount, why);
    if (blockmap == NULL)
    {
        free(b.segs);
        free(b.ss);
        free(b.nodes);
        return NULL;
    }

    memset(&out, 0, sizeof(out));
    memset(filepos, 0, sizeof(filepos));
    memset(size, 0, sizeof(size));
    {
        byte header[12] = { 'P', 'W', 'A', 'D' };
        put(&out, header, sizeof(header));
    }

    filepos[ML_LABEL] = (int)out.len;
    size[ML_LABEL] = 0;

    filepos[ML_THINGS] = (int)out.len;
    for (i = 0; i < m->nthings; i++)
    {
        mapthing_t t;
        t.x = (short)m->things[i].x;
        t.y = (short)m->things[i].y;
        t.angle = (short)m->things[i].angle;
        t.type = (short)m->things[i].type;
        t.options = (short)m->things[i].options;
        put(&out, &t, sizeof(t));
    }
    size[ML_THINGS] = (int)out.len - filepos[ML_THINGS];

    filepos[ML_LINEDEFS] = (int)out.len;
    for (i = 0; i < m->nlines; i++)
    {
        maplinedef_t l;
        l.v1 = (short)m->lines[i].v1;
        l.v2 = (short)m->lines[i].v2;
        l.flags = (short)m->lines[i].flags;
        l.special = (short)m->lines[i].special;
        l.tag = (short)m->lines[i].tag;
        l.sidenum[0] = (short)m->lines[i].side[0];
        l.sidenum[1] = (short)m->lines[i].side[1];
        put(&out, &l, sizeof(l));
    }
    size[ML_LINEDEFS] = (int)out.len - filepos[ML_LINEDEFS];

    filepos[ML_SIDEDEFS] = (int)out.len;
    for (i = 0; i < m->nsides; i++)
    {
        mapsidedef_t s;
        s.textureoffset = 0;
        s.rowoffset = 0;
        memcpy(s.toptexture, m->sides[i].top, 8);
        memcpy(s.bottomtexture, m->sides[i].bottom, 8);
        memcpy(s.midtexture, m->sides[i].mid, 8);
        s.sector = (short)m->sides[i].sector;
        put(&out, &s, sizeof(s));
    }
    size[ML_SIDEDEFS] = (int)out.len - filepos[ML_SIDEDEFS];

    filepos[ML_VERTEXES] = (int)out.len;
    for (i = 0; i < m->nverts; i++)
    {
        mapvertex_t v;
        v.x = (short)m->verts[i].x;
        v.y = (short)m->verts[i].y;
        put(&out, &v, sizeof(v));
    }
    size[ML_VERTEXES] = (int)out.len - filepos[ML_VERTEXES];

    filepos[ML_SEGS] = (int)out.len;
    for (i = 0; i < b.nsegs; i++)
    {
        mapseg_t s;
        int dx = m->verts[b.segs[i].v2].x - m->verts[b.segs[i].v1].x;
        int dy = m->verts[b.segs[i].v2].y - m->verts[b.segs[i].v1].y;

        s.v1 = (short)b.segs[i].v1;
        s.v2 = (short)b.segs[i].v2;
        s.angle = bam16(dx, dy);
        s.linedef = (short)b.segs[i].line;
        s.side = (short)b.segs[i].side;
        s.offset = (short)b.segs[i].offset;
        put(&out, &s, sizeof(s));
    }
    size[ML_SEGS] = (int)out.len - filepos[ML_SEGS];

    filepos[ML_SSECTORS] = (int)out.len;
    put(&out, b.ss, sizeof(mapsubsector_t) * (size_t)b.nss);
    size[ML_SSECTORS] = (int)out.len - filepos[ML_SSECTORS];

    filepos[ML_NODES] = (int)out.len;
    put(&out, b.nodes, sizeof(mapnode_t) * (size_t)b.nnodes);
    size[ML_NODES] = (int)out.len - filepos[ML_NODES];

    filepos[ML_SECTORS] = (int)out.len;
    for (i = 0; i < m->nsectors; i++)
    {
        mapsector_t s;
        s.floorheight = (short)m->sectors[i].floorheight;
        s.ceilingheight = (short)m->sectors[i].ceilingheight;
        memcpy(s.floorpic, m->sectors[i].floorpic, 8);
        memcpy(s.ceilingpic, m->sectors[i].ceilingpic, 8);
        s.lightlevel = (short)m->sectors[i].light;
        s.special = (short)m->sectors[i].special;
        s.tag = (short)m->sectors[i].tag;
        put(&out, &s, sizeof(s));
    }
    size[ML_SECTORS] = (int)out.len - filepos[ML_SECTORS];

    // Nothing is rejected: every sector can see every other. REJECT is an
    // optimisation for monster sight checks, and a zeroed one is correct.
    filepos[ML_REJECT] = (int)out.len;
    {
        int bytes = (m->nsectors * m->nsectors + 7) / 8;
        byte *zero = calloc((size_t)bytes, 1);
        put(&out, zero, (size_t)bytes);
        free(zero);
    }
    size[ML_REJECT] = (int)out.len - filepos[ML_REJECT];

    filepos[ML_BLOCKMAP] = (int)out.len;
    put(&out, blockmap, sizeof(short) * (size_t)blockmapcount);
    size[ML_BLOCKMAP] = (int)out.len - filepos[ML_BLOCKMAP];

    infotableofs = (int)out.len;
    for (i = 0; i < numlumps; i++)
    {
        byte entry[16];
        const char *nm = i == ML_LABEL ? lumpname : NULL;
        static const char *const names[] = {
            NULL, "THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES",
            "SEGS", "SSECTORS", "NODES", "SECTORS", "REJECT", "BLOCKMAP"
        };

        if (nm == NULL)
        {
            nm = names[i];
        }
        memset(entry, 0, sizeof(entry));
        memcpy(entry, &filepos[i], 4);
        memcpy(entry + 4, &size[i], 4);
        memcpy(entry + 8, nm, strlen(nm) < 8 ? strlen(nm) : 8);
        put(&out, entry, sizeof(entry));
    }

    dir = out.data;
    memcpy(dir + 4, &numlumps, 4);
    memcpy(dir + 8, &infotableofs, 4);

    free(blockmap);
    free(b.segs);
    free(b.ss);
    free(b.nodes);

    *len = out.len;
    return out.data;
}
