/*
 * 3GPP TS 26.245 Timed Text encoder
 * Copyright (c) 2012  Philip Langdale <philipl@overt.org>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdarg.h>
#include "avcodec.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/common.h"
#include "ass_split.h"
#include "ass.h"

#define STYLE_FLAG_BOLD         (1<<0)
#define STYLE_FLAG_ITALIC       (1<<1)
#define STYLE_FLAG_UNDERLINE    (1<<2)
#define STYLE_RECORD_SIZE       12
#define SIZE_ADD                10

#define STYL_BOX   (1<<0)
#define HLIT_BOX   (1<<1)
#define HCLR_BOX   (1<<2)

#define DEFAULT_STYLE_FONT_ID  0x01
#define DEFAULT_STYLE_FONTSIZE 0x12
#define DEFAULT_STYLE_COLOR    0xffffffff
#define DEFAULT_STYLE_FLAG     0x00

#define BGR_TO_RGB(c) (((c) & 0xff) << 16 | ((c) & 0xff00) | (((c) >> 16) & 0xff))
#define av_bprint_append_any(buf, data, size)   av_bprint_append_data(buf, ((const char*)data), size)

typedef struct {
    uint16_t style_start;
    uint16_t style_end;
    uint8_t style_flag;
    uint16_t style_fontID;
    uint8_t style_fontsize;
    uint32_t style_color;
} StyleBox;

typedef struct {
    uint16_t start;
    uint16_t end;
} HighlightBox;

typedef struct {
   uint32_t color;
} HilightcolorBox;

typedef struct {
    AVCodecContext *avctx;

    ASSSplitContext *ass_ctx;
    ASSStyle *ass_dialog_style;
    AVBPrint buffer;
    StyleBox **style_attributes;
    StyleBox *style_attributes_temp;
    HighlightBox hlit;
    HilightcolorBox hclr;
    int count;
    uint8_t box_flags;
    StyleBox d;
    uint16_t text_pos;
    uint16_t byte_count;
} MovTextContext;

typedef struct {
    uint32_t type;
    void (*encode)(MovTextContext *s, uint32_t tsmb_type);
} Box;

static void mov_text_cleanup(MovTextContext *s)
{
    int j;
    if (s->box_flags & STYL_BOX) {
        for (j = 0; j < s->count; j++) {
            av_freep(&s->style_attributes[j]);
        }
        av_freep(&s->style_attributes);
    }
    if (s->style_attributes_temp) {
        *s->style_attributes_temp = s->d;
    }
}

static void encode_styl(MovTextContext *s, uint32_t tsmb_type)
{
    int j;
    uint32_t tsmb_size;
    uint16_t style_entries;
    if ((s->box_flags & STYL_BOX) && s->count) {
        tsmb_size = s->count * STYLE_RECORD_SIZE + SIZE_ADD;
        tsmb_size = AV_RB32(&tsmb_size);
        style_entries = AV_RB16(&s->count);
        /*The above three attributes are hard coded for now
        but will come from ASS style in the future*/
        av_bprint_append_any(&s->buffer, &tsmb_size, 4);
        av_bprint_append_any(&s->buffer, &tsmb_type, 4);
        av_bprint_append_any(&s->buffer, &style_entries, 2);
        for (j = 0; j < s->count; j++) {
            uint16_t style_start, style_end, style_fontID;
            uint32_t style_color;

            style_start  = AV_RB16(&s->style_attributes[j]->style_start);
            style_end    = AV_RB16(&s->style_attributes[j]->style_end);
            style_color  = AV_RB32(&s->style_attributes[j]->style_color);
            style_fontID = AV_RB16(&s->style_attributes[j]->style_fontID);

            av_bprint_append_any(&s->buffer, &style_start, 2);
            av_bprint_append_any(&s->buffer, &style_end, 2);
            av_bprint_append_any(&s->buffer, &style_fontID, 2);
            av_bprint_append_any(&s->buffer, &s->style_attributes[j]->style_flag, 1);
            av_bprint_append_any(&s->buffer, &s->style_attributes[j]->style_fontsize, 1);
            av_bprint_append_any(&s->buffer, &style_color, 4);
        }
    }
    mov_text_cleanup(s);
}

static void encode_hlit(MovTextContext *s, uint32_t tsmb_type)
{
    uint32_t tsmb_size;
    uint16_t start, end;
    if (s->box_flags & HLIT_BOX) {
        tsmb_size = 12;
        tsmb_size = AV_RB32(&tsmb_size);
        start     = AV_RB16(&s->hlit.start);
        end       = AV_RB16(&s->hlit.end);
        av_bprint_append_any(&s->buffer, &tsmb_size, 4);
        av_bprint_append_any(&s->buffer, &tsmb_type, 4);
        av_bprint_append_any(&s->buffer, &start, 2);
        av_bprint_append_any(&s->buffer, &end, 2);
    }
}

static void encode_hclr(MovTextContext *s, uint32_t tsmb_type)
{
    uint32_t tsmb_size, color;
    if (s->box_flags & HCLR_BOX) {
        tsmb_size = 12;
        tsmb_size = AV_RB32(&tsmb_size);
        color     = AV_RB32(&s->hclr.color);
        av_bprint_append_any(&s->buffer, &tsmb_size, 4);
        av_bprint_append_any(&s->buffer, &tsmb_type, 4);
        av_bprint_append_any(&s->buffer, &color, 4);
    }
}

static const Box box_types[] = {
    { MKTAG('s','t','y','l'), encode_styl },
    { MKTAG('h','l','i','t'), encode_hlit },
    { MKTAG('h','c','l','r'), encode_hclr },
};

const static size_t box_count = FF_ARRAY_ELEMS(box_types);

static av_cold int mov_text_encode_init(AVCodecContext *avctx)
{
    /*
     * For now, we'll use a fixed default style. When we add styling
     * support, this will be generated from the ASS style.
     */
    static const uint8_t text_sample_entry[] = {
        0x00, 0x00, 0x00, 0x00, // uint32_t displayFlags
        0x01,                   // int8_t horizontal-justification
        0xFF,                   // int8_t vertical-justification
        0x00, 0x00, 0x00, 0x00, // uint8_t background-color-rgba[4]
        // BoxRecord {
        0x00, 0x00,             // int16_t top
        0x00, 0x00,             // int16_t left
        0x00, 0x00,             // int16_t bottom
        0x00, 0x00,             // int16_t right
        // };
        // StyleRecord {
        0x00, 0x00,             // uint16_t startChar
        0x00, 0x00,             // uint16_t endChar
        0x00, 0x01,             // uint16_t font-ID
        0x00,                   // uint8_t face-style-flags
        0x12,                   // uint8_t font-size
        0xFF, 0xFF, 0xFF, 0xFF, // uint8_t text-color-rgba[4]
        // };
        // FontTableBox {
        0x00, 0x00, 0x00, 0x12, // uint32_t size
        'f', 't', 'a', 'b',     // uint8_t name[4]
        0x00, 0x01,             // uint16_t entry-count
        // FontRecord {
        0x00, 0x01,             // uint16_t font-ID
        0x05,                   // uint8_t font-name-length
        'S', 'e', 'r', 'i', 'f',// uint8_t font[font-name-length]
        // };
        // };
    };

    MovTextContext *s = avctx->priv_data;
    s->avctx = avctx;

    s->style_attributes_temp = av_mallocz(sizeof(*s->style_attributes_temp));
    if (!s->style_attributes_temp) {
        return AVERROR(ENOMEM);
    }

    avctx->extradata_size = sizeof text_sample_entry;
    avctx->extradata = av_mallocz(avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!avctx->extradata)
        return AVERROR(ENOMEM);

    av_bprint_init(&s->buffer, 0, AV_BPRINT_SIZE_UNLIMITED);

    memcpy(avctx->extradata, text_sample_entry, avctx->extradata_size);

    s->ass_ctx = ff_ass_split(avctx->subtitle_header);

    // TODO: Initialize from ASS style record
    s->d.style_fontID   = DEFAULT_STYLE_FONT_ID;
    s->d.style_fontsize = DEFAULT_STYLE_FONTSIZE;
    s->d.style_color    = DEFAULT_STYLE_COLOR;
    s->d.style_flag     = DEFAULT_STYLE_FLAG;

    return s->ass_ctx ? 0 : AVERROR_INVALIDDATA;
}

// Start a new style box if needed
static int mov_text_style_start(MovTextContext *s)
{
    // there's an existing style entry
    if (s->style_attributes_temp->style_start == s->text_pos)
        // Still at same text pos, use same entry
        return 1;
    if (s->style_attributes_temp->style_flag     != s->d.style_flag  ||
        s->style_attributes_temp->style_color    != s->d.style_color ||
        s->style_attributes_temp->style_fontsize != s->d.style_fontsize) {
        // last style != defaults, end the style entry and start a new one
        s->box_flags |= STYL_BOX;
        s->style_attributes_temp->style_end = s->text_pos;
        av_dynarray_add(&s->style_attributes, &s->count, s->style_attributes_temp);
        s->style_attributes_temp = av_malloc(sizeof(*s->style_attributes_temp));
        if (!s->style_attributes_temp) {
            mov_text_cleanup(s);
            av_bprint_clear(&s->buffer);
            s->box_flags &= ~STYL_BOX;
            return 0;
        }

        *s->style_attributes_temp = s->d;
        s->style_attributes_temp->style_start = s->text_pos;
    } else { // style entry matches defaults, drop entry
        *s->style_attributes_temp = s->d;
        s->style_attributes_temp->style_start = s->text_pos;
    }
    return 1;
}

static uint8_t mov_text_style_to_flag(const char style)
{
    uint8_t style_flag = 0;

    switch (style){
    case 'b':
        style_flag = STYLE_FLAG_BOLD;
        break;
    case 'i':
        style_flag = STYLE_FLAG_ITALIC;
        break;
    case 'u':
        style_flag = STYLE_FLAG_UNDERLINE;
        break;
    }
    return style_flag;
}

static void mov_text_style_set(MovTextContext *s, uint8_t style_flags)
{
    if (!s->style_attributes_temp ||
        !((s->style_attributes_temp->style_flag & style_flags) ^ style_flags)) {
        // setting flags that that are already set
        return;
    }
    if (mov_text_style_start(s))
        s->style_attributes_temp->style_flag |= style_flags;
}

static void mov_text_style_cb(void *priv, const char style, int close)
{
    MovTextContext *s = priv;
    uint8_t style_flag = mov_text_style_to_flag(style);

    if (!s->style_attributes_temp ||
        !!(s->style_attributes_temp->style_flag & style_flag) != close) {
        // setting flag that is already set
        return;
    }
    if (mov_text_style_start(s)) {
        if (!close)
            s->style_attributes_temp->style_flag |= style_flag;
        else
            s->style_attributes_temp->style_flag &= ~style_flag;
    }
}

static void mov_text_color_set(MovTextContext *s, uint32_t color)
{
    if (!s->style_attributes_temp ||
        (s->style_attributes_temp->style_color & 0xffffff00) == color) {
        // color hasn't changed
        return;
    }
    if (mov_text_style_start(s))
        s->style_attributes_temp->style_color = (color & 0xffffff00) |
                            (s->style_attributes_temp->style_color & 0xff);
}

static void mov_text_color_cb(void *priv, unsigned int color, unsigned int color_id)
{
    MovTextContext *s = priv;

    color = BGR_TO_RGB(color) << 8;
    if (color_id == 1) {    //primary color changes
        mov_text_color_set(s, color);
    } else if (color_id == 2) {    //secondary color changes
        if (s->box_flags & HLIT_BOX) {  //close tag
            s->hlit.end = s->text_pos;
        } else {
            s->box_flags |= HCLR_BOX;
            s->box_flags |= HLIT_BOX;
            s->hlit.start = s->text_pos;
            s->hclr.color = color | 0xFF;  //set alpha value to FF
        }
    }
    /* If there are more than one secondary color changes in ASS, take start of
       first section and end of last section. Movtext allows only one
       highlight box per sample.
     */
}

static void mov_text_alpha_set(MovTextContext *s, uint8_t alpha)
{
    if (!s->style_attributes_temp ||
        (s->style_attributes_temp->style_color & 0xff) == alpha) {
        // color hasn't changed
        return;
    }
    if (mov_text_style_start(s))
        s->style_attributes_temp->style_color =
                (s->style_attributes_temp->style_color & 0xffffff00) | alpha;
}

static void mov_text_alpha_cb(void *priv, int alpha, int alpha_id)
{
    MovTextContext *s = priv;

    if (alpha_id == 1) // primary alpha changes
        mov_text_alpha_set(s, 255 - alpha);
}

static void mov_text_font_size_set(MovTextContext *s, int size)
{
    if (!s->style_attributes_temp ||
        s->style_attributes_temp->style_fontsize == size) {
        // color hasn't changed
        return;
    }
    if (mov_text_style_start(s))
        s->style_attributes_temp->style_fontsize = size;
}

static void mov_text_font_size_cb(void *priv, int size)
{
    mov_text_font_size_set((MovTextContext*)priv, size);
}

static void mov_text_end_cb(void *priv)
{
    // End of text, close any open style record
    mov_text_style_start((MovTextContext*)priv);
}

static void mov_text_ass_style_set(MovTextContext *s, ASSStyle *style)
{
    uint8_t    style_flags, alpha;
    uint32_t   color;

    if (style) {
        style_flags = (!!style->bold      * STYLE_FLAG_BOLD)   |
                      (!!style->italic    * STYLE_FLAG_ITALIC) |
                      (!!style->underline * STYLE_FLAG_UNDERLINE);
        mov_text_style_set(s, style_flags);
        color = BGR_TO_RGB(style->primary_color & 0xffffff) << 8;
        mov_text_color_set(s, color);
        alpha = 255 - ((uint32_t)style->primary_color >> 24);
        mov_text_alpha_set(s, alpha);
        mov_text_font_size_set(s, style->font_size);
    } else {
        // End current style record, go back to defaults
        mov_text_style_start(s);
    }
}

static void mov_text_dialog(MovTextContext *s, ASSDialog *dialog)
{
    ASSStyle * style = ff_ass_style_get(s->ass_ctx, dialog->style);

    s->ass_dialog_style = style;
    mov_text_ass_style_set(s, style);
}

static void mov_text_cancel_overrides_cb(void *priv, const char * style_name)
{
    MovTextContext *s = priv;
    ASSStyle * style;

    if (!style_name || !*style_name)
        style = s->ass_dialog_style;
    else
        style= ff_ass_style_get(s->ass_ctx, style_name);

    mov_text_ass_style_set(s, style);
}

static uint16_t utf8_strlen(const char *text, int len)
{
    uint16_t i = 0, ret = 0;
    while (i < len) {
        char c = text[i];
        if ((c & 0x80) == 0)
            i += 1;
        else if ((c & 0xE0) == 0xC0)
            i += 2;
        else if ((c & 0xF0) == 0xE0)
            i += 3;
        else if ((c & 0xF8) == 0xF0)
            i += 4;
        else
            return 0;
        ret++;
    }
    return ret;
}

static void mov_text_text_cb(void *priv, const char *text, int len)
{
    uint16_t utf8_len = utf8_strlen(text, len);
    MovTextContext *s = priv;
    av_bprint_append_data(&s->buffer, text, len);
    // If it's not utf-8, just use the byte length
    s->text_pos += utf8_len ? utf8_len : len;
    s->byte_count += len;
}

static void mov_text_new_line_cb(void *priv, int forced)
{
    MovTextContext *s = priv;
    av_bprint_append_data(&s->buffer, "\n", 1);
    s->text_pos += 1;
    s->byte_count += 1;
}

static const ASSCodesCallbacks mov_text_callbacks = {
    .text             = mov_text_text_cb,
    .new_line         = mov_text_new_line_cb,
    .style            = mov_text_style_cb,
    .color            = mov_text_color_cb,
    .alpha            = mov_text_alpha_cb,
    .font_size        = mov_text_font_size_cb,
    .cancel_overrides = mov_text_cancel_overrides_cb,
    .end              = mov_text_end_cb,
};

static int mov_text_encode_frame(AVCodecContext *avctx, unsigned char *buf,
                                 int bufsize, const AVSubtitle *sub)
{
    MovTextContext *s = avctx->priv_data;
    ASSDialog *dialog;
    int i, length;
    size_t j;

    s->byte_count = 0;
    s->text_pos = 0;
    s->count = 0;
    s->box_flags = 0;
    for (i = 0; i < sub->num_rects; i++) {
        const char *ass = sub->rects[i]->ass;

        if (sub->rects[i]->type != SUBTITLE_ASS) {
            av_log(avctx, AV_LOG_ERROR, "Only SUBTITLE_ASS type supported.\n");
            return AVERROR(ENOSYS);
        }

#if FF_API_ASS_TIMING
        if (!strncmp(ass, "Dialogue: ", 10)) {
            int num;
            dialog = ff_ass_split_dialog(s->ass_ctx, ass, 0, &num);
            for (; dialog && num--; dialog++) {
                mov_text_dialog(s, dialog);
                ff_ass_split_override_codes(&mov_text_callbacks, s, dialog->text);
            }
        } else {
#endif
            dialog = ff_ass_split_dialog2(s->ass_ctx, ass);
            if (!dialog)
                return AVERROR(ENOMEM);
            mov_text_dialog(s, dialog);
            ff_ass_split_override_codes(&mov_text_callbacks, s, dialog->text);
            ff_ass_free_dialog(&dialog);
#if FF_API_ASS_TIMING
        }
#endif

        for (j = 0; j < box_count; j++) {
            box_types[j].encode(s, box_types[j].type);
        }
    }

    AV_WB16(buf, s->byte_count);
    buf += 2;

    if (!av_bprint_is_complete(&s->buffer)) {
        length = AVERROR(ENOMEM);
        goto exit;
    }

    if (!s->buffer.len) {
        length = 0;
        goto exit;
    }

    if (s->buffer.len > bufsize - 3) {
        av_log(avctx, AV_LOG_ERROR, "Buffer too small for ASS event.\n");
        length = AVERROR(EINVAL);
        goto exit;
    }

    memcpy(buf, s->buffer.str, s->buffer.len);
    length = s->buffer.len + 2;

exit:
    av_bprint_clear(&s->buffer);
    return length;
}

static int mov_text_encode_close(AVCodecContext *avctx)
{
    MovTextContext *s = avctx->priv_data;
    ff_ass_split_free(s->ass_ctx);
    av_bprint_finalize(&s->buffer, NULL);
    return 0;
}

AVCodec ff_movtext_encoder = {
    .name           = "mov_text",
    .long_name      = NULL_IF_CONFIG_SMALL("3GPP Timed Text subtitle"),
    .type           = AVMEDIA_TYPE_SUBTITLE,
    .id             = AV_CODEC_ID_MOV_TEXT,
    .priv_data_size = sizeof(MovTextContext),
    .init           = mov_text_encode_init,
    .encode_sub     = mov_text_encode_frame,
    .close          = mov_text_encode_close,
};
