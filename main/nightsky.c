/* The night sky. See nightsky.h for why this is drawn and not a picture. */
#include "nightsky.h"

#define SCR   466
#define CX    233
#define CY    233

/* ── the stars ────────────────────────────────────────────────
 * Positions come from a hash rather than a table, so a few hundred stars cost
 * no flash at all. The same seed gives the same sky every boot — a wallpaper
 * that rearranges itself when you turn the page would be unsettling. */
static uint32_t hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

#define STARS      210     /* over the whole disc */
#define HAZE_STARS 150     /* the extra ones crowding the milky way */

/* The milky way runs corner to corner. A band, not a line: stars scatter
 * around it and thin out with distance. */
#define BAND_DX    0.64f   /* unit vector along the band */
#define BAND_DY   -0.77f

static void sky_draw(lv_event_t *e)
{
    lv_obj_t   *o     = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);

    lv_area_t box;
    lv_obj_get_coords(o, &box);
    const int ox = box.x1, oy = box.y1;

    lv_draw_rect_dsc_t d;

    /* 🚨 The milky way was three soft ellipses laid along the band. Every one
     * of them is drawn upright, so together they read as a horizontal smear
     * across the middle of the screen — a seam, not a galaxy. It is crowded
     * dim stars instead, which fall on the diagonal because their positions
     * do. */

    for (int i = 0; i < STARS + HAZE_STARS; i++) {
        uint32_t h  = hash((uint32_t)i * 2654435761u + 12345u);
        uint32_t h2 = hash(h);

        int x, y;
        if (i < STARS) {
            x = (int)(h  % SCR);
            y = (int)(h2 % SCR);
        } else {
            /* Crowded onto the band: a long step along it, a short one across */
            float along  = ((float)(h  % 1000) / 1000.0f - 0.5f) * 520.0f;
            float across = ((float)(h2 % 1000) / 1000.0f - 0.5f) * 108.0f;
            across *= (float)(h2 % 7 + 1) / 7.0f;   /* crowded at the spine, thin at the edges */
            x = CX + (int)(BAND_DX * along - BAND_DY * across);
            y = CY + (int)(BAND_DY * along + BAND_DX * across);
        }

        /* Anything outside the round screen is wasted drawing */
        int dx = x - CX, dy = y - CY;
        if (dx * dx + dy * dy > 232 * 232) continue;

        uint32_t h3 = hash(h2 ^ 0x9E3779B9u);
        int      r  = (int)(h3 % 100);

        /* Mostly faint. A sky of equally bright dots reads as noise, not sky. */
        int size, opa;
        if      (r < 62) { size = 1; opa =  50 + (int)(h3 >> 8) % 60; }
        else if (r < 90) { size = 1; opa = 130 + (int)(h3 >> 8) % 70; }
        else             { size = 2; opa = 210 + (int)(h3 >> 8) % 45; }
        if (i >= STARS) opa = opa * 3 / 5;      /* the band's own stars are dimmer */

        /* A few warm, a few cool, the rest white. Real skies are not monochrome. */
        uint32_t col = 0xFFFFFF;
        switch ((h3 >> 16) % 12) {
        case 0: case 1: col = 0xFFE3C0; break;   /* warm */
        case 2:         col = 0xFFCF9E; break;
        case 3: case 4: col = 0xCFE0FF; break;   /* cool */
        default: break;
        }

        lv_draw_rect_dsc_init(&d);
        d.radius   = LV_RADIUS_CIRCLE;
        d.bg_color = lv_color_hex(col);
        d.bg_opa   = (lv_opa_t)(opa > 255 ? 255 : opa);
        lv_area_t a = { ox + x, oy + y, ox + x + size, oy + y + size };
        lv_draw_rect(layer, &d, &a);

        /* The brightest few get a halo, which is what sells them as bright */
        if (size == 2 && (h3 >> 24) % 3 == 0) {
            lv_draw_rect_dsc_init(&d);
            d.radius   = LV_RADIUS_CIRCLE;
            d.bg_color = lv_color_hex(col);
            d.bg_opa   = 40;
            lv_area_t g = { ox + x - 3, oy + y - 3, ox + x + size + 3, oy + y + size + 3 };
            lv_draw_rect(layer, &d, &g);
        }
    }
}

lv_obj_t *nightsky_create(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    /* 🚨 lv_obj_create() is clickable by default. Left that way the wallpaper
     * is the pressed object for every swipe on the bare background, the screen
     * never sees the gesture, and the home pages stop turning. */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, SCR, SCR);
    lv_obj_center(o);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);

    /* 🚨 Flat, not a gradient. The panel is RGB565, so blue moves in 32 steps
     * across 466 rows — a gradient from 0x02040C to 0x0B1330 crosses five of
     * them and each crossing is a hard horizontal line through the middle of
     * the wallpaper. Depth comes from where the stars are instead, which costs
     * nothing and cannot band.
     *
     * A soft glow low down was tried instead, built from concentric circles so
     * that any banding would follow the curve of the screen rather than cut
     * across it. At an opacity you could actually see, the circles themselves
     * became visible as rings. Flat is better than nearly-flat with rings.
     *
     * Near-black is also the cheapest thing an AMOLED can show. */
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x03050E), 0);

    lv_obj_add_event_cb(o, sky_draw, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}
