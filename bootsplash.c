// Initialize the VGA console and possibly show a boot splash image.
//
// Copyright (C) 2009-2010  coresystems GmbH
// Copyright (C) 2010  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "bregs.h" // struct bregs
#include "config.h" // CONFIG_*
#include "farptr.h" // FLATPTR_TO_SEG
#include "malloc.h" // free
#include "output.h" // dprintf
#include "romfile.h" // romfile_loadfile
#include "stacks.h" // call16_int
#include "std/vbe.h" // struct vbe_info
#include "string.h" // memset
#include "util.h" // enable_bootsplash

#define off_t long

enum whence {
    SEEK_SET,
    SEEK_CUR
};

enum file_type {
    JPEG,
    BMP,
    GIF
};

typedef struct gd_Palette {
    int size;
    u8 colors[0x100 * 3];
} gd_Palette;

typedef struct gd_GCE {
    u16 delay;
    u8 tindex;
    u8 disposal;
    int input;
    int transparency;
} gd_GCE;

typedef struct gd_GIF {
    u8 *filedata;
    off_t filedata_offset;
    off_t anim_start;
    u16 width, height;
    u16 depth;
    u16 loop_count;
    gd_GCE gce;
    gd_Palette *palette;
    gd_Palette lct, gct;
    void (*plain_text)(
        struct gd_GIF *gif, u16 tx, u16 ty,
        u16 tw, u16 th, u8 cw, u8 ch,
        u8 fg, u8 bg
    );
    void (*comment)(struct gd_GIF *gif);
    void (*application)(struct gd_GIF *gif, char id[8], char auth[3]);
    u16 fx, fy, fw, fh;
    u8 bgindex;
    u8 *canvas, *frame;
} gd_GIF;

typedef struct rendering_data {
    gd_GIF *gif;
    void *framebuffer;
    u8 *picture;
    int imagesize;
} rendering_data;

#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define MAX(A, B) ((A) > (B) ? (A) : (B))

typedef struct Entry {
    u16 length;
    u16 prefix;
    u8  suffix;
} Entry;

typedef struct Table {
    int bulk;
    int nentries;
    Entry *entries;
} Table;

static size_t
gd_read(gd_GIF *gif, void *dst, size_t len)
{
    memcpy(dst, gif->filedata + gif->filedata_offset, len);
    gif->filedata_offset += len;
    return len;
}


static off_t
gd_lseek(gd_GIF *gif, off_t offset, int whence)
{
    switch (whence) {
        case SEEK_SET:
            gif->filedata_offset = offset;
            break;
        case SEEK_CUR:
            gif->filedata_offset += offset;
            break;
        default:
            dprintf(1, "fdmap_lseek whence %d not implemented", whence);
            return -1;
    }
    return gif->filedata_offset;
}

static u16
read_num(gd_GIF *gif)
{
    u8 bytes[2];

    gd_read(gif, bytes, 2);
    return bytes[0] + (((u16) bytes[1]) << 8);
}

static gd_GIF *
gd_decode_gif(u8 *filedata)
{
    u8 sigver[3];
    u16 width, height, depth;
    u8 fdsz, bgidx, aspect;
    int i;
    u8 *bgcolor;
    int gct_sz;
    gd_GIF *gif;

    /* Create gd_GIF Structure. */
    gif = malloc_tmphigh(sizeof(*gif));
    memset(gif, 0, sizeof(*gif));
    if (!gif) goto fail;
    gif->filedata = filedata;

    /* Header */
    gd_read(gif, sigver, 3);
    if (memcmp(sigver, "GIF", 3) != 0) {
        dprintf(1, "invalid signature\n");
        goto fail;
    }
    /* Version */
    gd_read(gif, sigver, 3);
    if (memcmp(sigver, "89a", 3) != 0) {
        dprintf(1, "invalid version\n");
        goto fail;
    }
    /* Width x Height */
    width  = read_num(gif);
    height = read_num(gif);
    /* FDSZ */
    gd_read(gif, &fdsz, 1);
    /* Presence of GCT */
    if (!(fdsz & 0x80)) {
        dprintf(1, "no global color table\n");
        goto fail;
    }
    /* Color Space's Depth */
    depth = ((fdsz >> 4) & 7) + 1;
    /* Ignore Sort Flag. */
    /* GCT Size */
    gct_sz = 1 << ((fdsz & 0x07) + 1);
    /* Background Color Index */
    gd_read(gif, &bgidx, 1);
    /* Aspect Ratio */
    gd_read(gif, &aspect, 1);
    
    gif->width  = width;
    gif->height = height;
    gif->depth  = depth;
    /* Read GCT */
    gif->gct.size = gct_sz;
    gd_read(gif, gif->gct.colors, 3 * gif->gct.size);
    gif->palette = &gif->gct;
    gif->bgindex = bgidx;
    gif->frame = malloc_tmphigh(4 * width * height);
    memset(gif->frame, 0, 4 * width * height);
    if (!gif->frame) {
        free(gif);
        goto fail;
    }
    gif->canvas = &gif->frame[width * height];
    if (gif->bgindex)
        memset(gif->frame, gif->bgindex, gif->width * gif->height);
    bgcolor = &gif->palette->colors[gif->bgindex*3];
    if (bgcolor[0] || bgcolor[1] || bgcolor [2])
        for (i = 0; i < gif->width * gif->height; i++)
            memcpy(&gif->canvas[i*3], bgcolor, 3);
    gif->anim_start = gd_lseek(gif, 0, SEEK_CUR);
    goto ok;
fail:
    //free(gif->filedata);
    return NULL;
ok:
    return gif;
}

static void
discard_sub_blocks(gd_GIF *gif)
{
    u8 size;

    do {
        gd_read(gif, &size, 1);
        gd_lseek(gif, size, SEEK_CUR);
    } while (size);
}

static void
read_plain_text_ext(gd_GIF *gif)
{
    if (gif->plain_text) {
        u16 tx, ty, tw, th;
        u8 cw, ch, fg, bg;
        off_t sub_block;
        gd_lseek(gif, 1, SEEK_CUR); /* block size = 12 */
        tx = read_num(gif);
        ty = read_num(gif);
        tw = read_num(gif);
        th = read_num(gif);
        gd_read(gif, &cw, 1);
        gd_read(gif, &ch, 1);
        gd_read(gif, &fg, 1);
        gd_read(gif, &bg, 1);
        sub_block = gd_lseek(gif, 0, SEEK_CUR);
        gif->plain_text(gif, tx, ty, tw, th, cw, ch, fg, bg);
        gd_lseek(gif, sub_block, SEEK_SET);
    } else {
        /* Discard plain text metadata. */
        gd_lseek(gif, 13, SEEK_CUR);
    }
    /* Discard plain text sub-blocks. */
    discard_sub_blocks(gif);
}

static void
read_graphic_control_ext(gd_GIF *gif)
{
    u8 rdit;

    /* Discard block size (always 0x04). */
    gd_lseek(gif, 1, SEEK_CUR);
    gd_read(gif, &rdit, 1);
    gif->gce.disposal = (rdit >> 2) & 3;
    gif->gce.input = rdit & 2;
    gif->gce.transparency = rdit & 1;
    gif->gce.delay = read_num(gif);
    gd_read(gif, &gif->gce.tindex, 1);
    /* Skip block terminator. */
    gd_lseek(gif, 1, SEEK_CUR);
}

static void
read_comment_ext(gd_GIF *gif)
{
    if (gif->comment) {
        off_t sub_block = gd_lseek(gif, 0, SEEK_CUR);
        gif->comment(gif);
        gd_lseek(gif, sub_block, SEEK_SET);
    }
    /* Discard comment sub-blocks. */
    discard_sub_blocks(gif);
}

static void
read_application_ext(gd_GIF *gif)
{
    char app_id[8];
    char app_auth_code[3];

    /* Discard block size (always 0x0B). */
    gd_lseek(gif, 1, SEEK_CUR);
    /* Application Identifier. */
    gd_read(gif, app_id, 8);
    /* Application Authentication Code. */
    gd_read(gif, app_auth_code, 3);
    if (memcmp(app_id, "NETSCAPE", 8) != 0) {
        /* Discard block size (0x03) and constant byte (0x01). */
        gd_lseek(gif, 2, SEEK_CUR);
        gif->loop_count = read_num(gif);
        /* Skip block terminator. */
        gd_lseek(gif, 1, SEEK_CUR);
    } else if (gif->application) {
        off_t sub_block = gd_lseek(gif, 0, SEEK_CUR);
        gif->application(gif, app_id, app_auth_code);
        gd_lseek(gif, sub_block, SEEK_SET);
        discard_sub_blocks(gif);
    } else {
        discard_sub_blocks(gif);
    }
}

static void
read_ext(gd_GIF *gif)
{
    u8 label;

    gd_read(gif, &label, 1);
    switch (label) {
    case 0x01:
        read_plain_text_ext(gif);
        break;
    case 0xF9:
        read_graphic_control_ext(gif);
        break;
    case 0xFE:
        read_comment_ext(gif);
        break;
    case 0xFF:
        read_application_ext(gif);
        break;
    default:
        dprintf(1, "unknown extension: %02X\n", label);
    }
}

static Table *
new_table(int key_size)
{
    int key;
    int init_bulk = MAX(1 << (key_size + 1), 0x100);
    Table *table = malloc_tmphigh(sizeof(*table) + sizeof(Entry) * init_bulk);
    if (table) {
        table->bulk = init_bulk;
        table->nentries = (1 << key_size) + 2;
        table->entries = (Entry *) &table[1];
        for (key = 0; key < (1 << key_size); key++)
            table->entries[key] = (Entry) {1, 0xFFF, key};

        return table;     
    } else {
        return NULL;
    }
}

/* Add table entry. Return value:
 *  0 on success
 *  +1 if key size must be incremented after this addition
 *  -1 if could not realloc table */
static int
add_entry(Table **tablep, u16 length, u16 prefix, u8 suffix) {
    Table *table = *tablep;
    Table *new_table;
    int old_size;

    if (!table)
        return -1;

    if (table->nentries == table->bulk) {
        old_size = sizeof(*table) + sizeof(Entry) * table->bulk;
        table->bulk *= 2;
        new_table = malloc_tmphigh(sizeof(*table) + sizeof(Entry) * table->bulk);
        if (!new_table) {
            warn_noalloc();
            return -1;
        }
        memcpy(new_table, table, old_size);
        free(table);
        table = new_table;
        table->entries = (Entry *)&table[1];
        *tablep = table;
    }
    table->entries[table->nentries] = (Entry){length, prefix, suffix};
    table->nentries++;
    if ((table->nentries & (table->nentries - 1)) == 0)
        return 1;
    return 0;
}

static u16
get_key(gd_GIF *gif, int key_size, u8 *sub_len, u8 *shift, u8 *byte)
{
    int bits_read;
    int rpad;
    int frag_size;
    u16 key;

    key = 0;
    for (bits_read = 0; bits_read < key_size; bits_read += frag_size) {
        rpad = (*shift + bits_read) % 8;
        if (rpad == 0) {
            /* Update byte. */
            if (*sub_len == 0) {
                gd_read(gif, sub_len, 1); /* Must be nonzero! */
                if (*sub_len == 0)
                    return 0x1000;
            }
            gd_read(gif, byte, 1);
            (*sub_len)--;
        }
        frag_size = MIN(key_size - bits_read, 8 - rpad);
        key |= ((u16) ((*byte) >> rpad)) << bits_read;
    }
    /* Clear extra bits to the left. */
    key &= (1 << key_size) - 1;
    *shift = (*shift + key_size) % 8;
    return key;
}

/* Compute output index of y-th input line, in frame of height h. */
static int
interlaced_line_index(int h, int y)
{
    int p; /* number of lines in current pass */

    p = (h - 1) / 8 + 1;
    if (y < p) /* pass 1 */
        return y * 8;
    y -= p;
    p = (h - 5) / 8 + 1;
    if (y < p) /* pass 2 */
        return y * 8 + 4;
    y -= p;
    p = (h - 3) / 4 + 1;
    if (y < p) /* pass 3 */
        return y * 4 + 2;
    y -= p;
    /* pass 4 */
    return y * 2 + 1;
}

/* Decompress image pixels.
 * Return 0 on success or -1 on out-of-memory (w.r.t. LZW code table). */
static int
read_image_data(gd_GIF *gif, int interlace)
{
    u8 sub_len, shift, byte;
    int init_key_size, key_size, table_is_full;
    int frm_off, frm_size, str_len, i, p, x, y;
    u16 key, clear, stop;
    int ret;
    Table *table;
    Entry entry;
    off_t start, end;

    gd_read(gif, &byte, 1);
    key_size = (int) byte;
    if (key_size < 2 || key_size > 8)
        return -1;
    
    start = gd_lseek(gif, 0, SEEK_CUR);
    discard_sub_blocks(gif);
    end = gd_lseek(gif, 0, SEEK_CUR);
    gd_lseek(gif, start, SEEK_SET);
    clear = 1 << key_size;
    stop = clear + 1;
    table = new_table(key_size);
    if (!table) {
        warn_noalloc();
        return -1;
    }
    key_size++;
    init_key_size = key_size;
    sub_len = shift = 0;
    key = get_key(gif, key_size, &sub_len, &shift, &byte); /* clear code */
    frm_off = 0;
    ret = 0;
    frm_size = gif->fw*gif->fh;
    while (frm_off < frm_size) {
        if (key == clear) {
            key_size = init_key_size;
            table->nentries = (1 << (key_size - 1)) + 2;
            table_is_full = 0;
        } else if (!table_is_full) {
            ret = add_entry(&table, str_len + 1, key, entry.suffix);
            if (ret == -1) {
                free(table);
                return -1;
            }
            if (table->nentries == 0x1000) {
                ret = 0;
                table_is_full = 1;
            }
        }
        key = get_key(gif, key_size, &sub_len, &shift, &byte);
        if (key == clear) continue;
        if (key == stop || key == 0x1000) break;
        if (ret == 1) key_size++;
        entry = table->entries[key];
        str_len = entry.length;
        for (i = 0; i < str_len; i++) {
            p = frm_off + entry.length - 1;
            x = p % gif->fw;
            y = p / gif->fw;
            if (interlace)
                y = interlaced_line_index((int) gif->fh, y);
            gif->frame[(gif->fy + y) * gif->width + gif->fx + x] = entry.suffix;
            if (entry.prefix == 0xFFF)
                break;
            else
                entry = table->entries[entry.prefix];
        }
        frm_off += str_len;
        if (key < table->nentries - 1 && !table_is_full)
            table->entries[table->nentries - 1].suffix = entry.suffix;
    }
    free(table);
    if (key == stop)
        gd_read(gif, &sub_len, 1); /* Must be zero! */
    gd_lseek(gif, end, SEEK_SET);
    return 0;
}

/* Read image.
 * Return 0 on success or -1 on out-of-memory (w.r.t. LZW code table). */
static int
read_image(gd_GIF *gif)
{
    u8 fisrz;
    int interlace;

    /* Image Descriptor. */
    gif->fx = read_num(gif);
    gif->fy = read_num(gif);
    
    if (gif->fx >= gif->width || gif->fy >= gif->height)
        return -1;
    
    gif->fw = read_num(gif);
    gif->fh = read_num(gif);
    
    gif->fw = MIN(gif->fw, gif->width - gif->fx);
    gif->fh = MIN(gif->fh, gif->height - gif->fy);
    
    gd_read(gif, &fisrz, 1);
    interlace = fisrz & 0x40;
    /* Ignore Sort Flag. */
    /* Local Color Table? */
    if (fisrz & 0x80) {
        /* Read LCT */
        gif->lct.size = 1 << ((fisrz & 0x07) + 1);
        gd_read(gif, gif->lct.colors, 3 * gif->lct.size);
        gif->palette = &gif->lct;
    } else
        gif->palette = &gif->gct;
    /* Image Data. */
    return read_image_data(gif, interlace);
}

static void
render_frame_rect(gd_GIF *gif, u8 *buffer)
{
    int i, j, k;
    u8 index, *color;
    i = gif->fy * gif->width + gif->fx;
    for (j = 0; j < gif->fh; j++) {
        for (k = 0; k < gif->fw; k++) {
            index = gif->frame[(gif->fy + j) * gif->width + gif->fx + k];
            color = &gif->palette->colors[index*3];
            if (!gif->gce.transparency || index != gif->gce.tindex)
                 memcpy(&buffer[(i+k)*3], color, 3);
        }
        i += gif->width;
    }
}

static void
render_frame_rect_bpp_32(gd_GIF *gif, u8 *buffer)
{
    int i, j, k;
    u8 index, *color;
    i = gif->fy * gif->width + gif->fx;
    for (j = 0; j < gif->fh; j++) {
        for (k = 0; k < gif->fw; k++) {
            index = gif->frame[(gif->fy + j) * gif->width + gif->fx + k];
            color = &gif->palette->colors[index*3];
            if (!gif->gce.transparency || index != gif->gce.tindex) {
                buffer[(i + k) * 4] = color[2];
                buffer[(i + k) * 4 + 1] = color[1];
                buffer[(i + k) * 4 + 2] = color[0];
            }
        }
        i += gif->width;
    }
}


static void
dispose(gd_GIF *gif)
{
    int i, j, k;
    u8 *bgcolor;
    switch (gif->gce.disposal) {
    case 2: /* Restore to background color. */
        bgcolor = &gif->palette->colors[gif->bgindex*3];
        i = gif->fy * gif->width + gif->fx;
        for (j = 0; j < gif->fh; j++) {
            for (k = 0; k < gif->fw; k++)
                memcpy(&gif->canvas[(i+k)*3], bgcolor, 3);
            i += gif->width;
        }
        break;
    case 3: /* Restore to previous, i.e., don't update canvas.*/
        break;
    default:
        /* Add frame non-transparent pixels to canvas. */
        render_frame_rect(gif, gif->canvas);
    }
}

/* Return 1 if got a frame; 0 if got GIF trailer; -1 if error. */
static int
gd_get_frame(gd_GIF *gif)
{
    char sep;

    dispose(gif);
    gd_read(gif, &sep, 1);
    while (sep != ',') {
        if (sep == ';')
            return 0;
        if (sep == '!')
            read_ext(gif);
        else return -1;
        gd_read(gif, &sep, 1);
    }
    if (read_image(gif) == -1)
        return -1;
    return 1;
}

static void
gd_render_frame(gd_GIF *gif, u8 *buffer)
{
    memcpy(buffer, gif->canvas, gif->width * gif->height * 3);
    render_frame_rect(gif, buffer);
}

static int
gd_is_bgcolor(gd_GIF *gif, u8 color[3])
{
    return !memcmp(&gif->palette->colors[gif->bgindex*3], color, 3);
}

static void
gd_rewind(gd_GIF *gif)
{
    gd_lseek(gif, gif->anim_start, SEEK_SET);
}

static void
gd_close_gif(gd_GIF *gif)
{
    free(gif->frame);    
    free(gif);
}

/****************************************************************
 * Helper functions
 ****************************************************************/

// Call int10 vga handler.
static void
call16_int10(struct bregs *br)
{
    br->flags = F_IF;
    start_preempt();
    call16_int(0x10, br);
    finish_preempt();
}


/****************************************************************
 * VGA text / graphics console
 ****************************************************************/

void
enable_vga_console(void)
{
    dprintf(1, "Turning on vga text mode console\n");
    struct bregs br;

    /* Enable VGA text mode */
    memset(&br, 0, sizeof(br));
    br.ax = 0x0003;
    call16_int10(&br);

    // Write to screen.
    printf("SeaBIOS (version %s)\n", VERSION);
    display_uuid();
}

static int
find_videomode(struct vbe_info *vesa_info, struct vbe_mode_info *mode_info
               , int width, int height, int bpp_req)
{
    dprintf(3, "Finding vesa mode with dimensions %d/%d\n", width, height);
    u16 *videomodes = SEGOFF_TO_FLATPTR(vesa_info->video_mode);
    for (;; videomodes++) {
        u16 videomode = *videomodes;
        if (videomode == 0xffff) {
            dprintf(1, "Unable to find vesa video mode dimensions %d/%d\n"
                    , width, height);
            return -1;
        }
        struct bregs br;
        memset(&br, 0, sizeof(br));
        br.ax = 0x4f01;
        br.cx = videomode;
        br.di = FLATPTR_TO_OFFSET(mode_info);
        br.es = FLATPTR_TO_SEG(mode_info);
        call16_int10(&br);
        if (br.ax != 0x4f) {
            dprintf(1, "get_mode failed.\n");
            continue;
        }
         
        if (mode_info->xres != width
            || mode_info->yres != height)
            continue;
        u8 depth = mode_info->bits_per_pixel;
        if (bpp_req == 0) {
            if ((depth != 16 && depth != 24 && depth != 32)
                || mode_info->green_size == 5)
                continue;
        } else {
            if (depth != bpp_req)
                continue;
        }
        return videomode;
    }
}

static volatile int BootsplashActive = 0;

void
animated_bootsplash(void *data) {
    struct rendering_data *rdata = (struct rendering_data*)data;

    while (BootsplashActive) {
        u32 timer_end = timer_calc(rdata->gif->gce.delay);
        
        int ret = gd_get_frame(rdata->gif);
        
        if (ret == -1)
            break;
        
        render_frame_rect_bpp_32(rdata->gif, rdata->picture);
        iomemcpy(rdata->framebuffer, rdata->picture, rdata->imagesize);
        
        if (ret == 0) {
            gd_rewind(rdata->gif);
        }

        while (!timer_check(timer_end)) {
            if (BootsplashActive == 0)
                break;
            yield();
        }
    }

    free(rdata->gif->filedata);
    free(rdata->picture);
    gd_close_gif(rdata->gif);
}

void
enable_bootsplash(void)
{
    if (!CONFIG_BOOTSPLASH)
        return;
    /* splash picture can be jpeg, bmp or gif file */
    dprintf(3, "Checking for bootsplash\n");
    u8 type = JPEG;
    int filesize;
    u8 *filedata = romfile_loadfile("bootsplash.jpg", &filesize);
    if (!filedata) {
        filedata = romfile_loadfile("bootsplash.bmp", &filesize);
        type = BMP;
        if (!filedata) {
            filedata = romfile_loadfile("bootsplash.gif", &filesize);
            if (!filedata) {
                return;
            }
            type = GIF;
        }
    }
    dprintf(3, "start showing bootsplash\n");

    u8 *picture = NULL; /* data buff used to be flushed to the video buf */
    struct jpeg_decdata *jpeg = NULL;
    struct bmp_decdata *bmp = NULL;
    struct gd_GIF *gif = NULL;
    struct vbe_info *vesa_info = malloc_tmplow(sizeof(*vesa_info));
    struct vbe_mode_info *mode_info = malloc_tmplow(sizeof(*mode_info));
    if (!vesa_info || !mode_info) {
        warn_noalloc();
        goto done;
    }

    /* Check whether we have a VESA 2.0 compliant BIOS */
    memset(vesa_info, 0, sizeof(struct vbe_info));
    vesa_info->signature = VBE2_SIGNATURE;
    struct bregs br;
    memset(&br, 0, sizeof(br));
    br.ax = 0x4f00;
    br.di = FLATPTR_TO_OFFSET(vesa_info);
    br.es = FLATPTR_TO_SEG(vesa_info);
    call16_int10(&br);
    if (vesa_info->signature != VESA_SIGNATURE) {
        dprintf(1,"No VBE2 found.\n");
        goto done;
    }

    /* Print some debugging information about our card. */
    char *vendor = SEGOFF_TO_FLATPTR(vesa_info->oem_vendor_string);
    char *product = SEGOFF_TO_FLATPTR(vesa_info->oem_product_string);
    dprintf(3, "VESA %d.%d\nVENDOR: %s\nPRODUCT: %s\n",
            vesa_info->version>>8, vesa_info->version&0xff,
            vendor, product);

    int ret, width, height;
    int bpp_require = 0;
    
    switch (type) {
        case JPEG:
            jpeg = jpeg_alloc();
            if (!jpeg) {
                warn_noalloc();
                goto done;
            }
            /* Parse jpeg and get image size. */
            dprintf(5, "Decoding bootsplash.jpg\n");
            ret = jpeg_decode(jpeg, filedata);
            if (ret) {
                dprintf(1, "jpeg_decode failed with return code %d...\n", ret);
                goto done;
            }
            jpeg_get_size(jpeg, &width, &height);
            break;
        case BMP:
            bmp = bmp_alloc();
            if (!bmp) {
                warn_noalloc();
                goto done;
            }
            /* Parse bmp and get image size. */
            dprintf(5, "Decoding bootsplash.bmp\n");
            ret = bmp_decode(bmp, filedata, filesize);
            if (ret) {
                dprintf(1, "bmp_decode failed with return code %d...\n", ret);
                goto done;
            }
            bmp_get_info(bmp, &width, &height, &bpp_require);
            break;
        case GIF:
            gif = gd_decode_gif(filedata);
            
            if (!gif) {
                dprintf(1, "gif_decode failed");
                goto done;
            }
            
            width = gif->width;
            height = gif->height;
            bpp_require = 32;
            break;
    }

    // jpeg would use 16 or 24 bpp video mode, BMP uses 16/24/32 bpp mode.

    // Try to find a graphics mode with the corresponding dimensions.
    int videomode = find_videomode(vesa_info, mode_info, width, height,
                                       bpp_require);
    if (videomode < 0) {
        dprintf(1, "failed to find a videomode with %dx%d %dbpp (0=any).\n",
                    width, height, bpp_require);
        goto done;
    }
    void *framebuffer = (void *)mode_info->phys_base;
    int depth = mode_info->bits_per_pixel;
    dprintf(3, "mode: %04x\n", videomode);
    dprintf(3, "framebuffer: %p\n", framebuffer);
    dprintf(3, "bytes per scanline: %d\n", mode_info->bytes_per_scanline);
    dprintf(3, "bits per pixel: %d\n", depth);

    // Allocate space for image and decompress it.
    int imagesize = height * mode_info->bytes_per_scanline;

    picture = malloc_tmphigh(imagesize);
    if (!picture) {
        warn_noalloc();
        goto done;
    }
    
    switch (type) {
        case JPEG:
            dprintf(5, "Decompressing bootsplash.jpg\n");
            ret = jpeg_show(jpeg, picture, width, height, depth,
                                mode_info->bytes_per_scanline);
            if (ret) {
                dprintf(1, "jpeg_show failed with return code %d...\n", ret);
                goto done;
            }
            break;
        case BMP:
            dprintf(5, "Decompressing bootsplash.bmp\n");
            ret = bmp_show(bmp, picture, width, height, depth,
                            mode_info->bytes_per_scanline);
            if (ret) {
                dprintf(1, "bmp_show failed with return code %d...\n", ret);
                goto done;
            }
            break;
        default:
            break;
    }

    /* Switch to graphics mode */
    dprintf(5, "Switching to graphics mode\n");
    memset(&br, 0, sizeof(br));
    br.ax = 0x4f02;
    br.bx = videomode | VBE_MODE_LINEAR_FRAME_BUFFER;
    call16_int10(&br);
    if (br.ax != 0x4f) {
        dprintf(1, "set_mode failed.\n");
        goto done;
    }

    /* Show the picture */
    dprintf(5, "Showing bootsplash picture\n");
    BootsplashActive = 1;
    if (type == GIF) {
        struct rendering_data data = {
            gif,
            framebuffer,
            picture,
            imagesize
        };
        
        run_thread(animated_bootsplash, &data);
    } else {
        iomemcpy(framebuffer, picture, imagesize);
        
    }
    dprintf(5, "Bootsplash copy complete\n");

done:
    if (type != GIF || BootsplashActive == 0) {
        free(picture);
        free(filedata);
    }
    
    free(vesa_info);
    free(mode_info);
    free(jpeg);
    free(bmp);
    return;
}

void
disable_bootsplash(void)
{
    if (!CONFIG_BOOTSPLASH || !BootsplashActive)
        return;
    BootsplashActive = 0;
    wait_threads();
    enable_vga_console();
}
