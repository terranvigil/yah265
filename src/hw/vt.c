/*
 * vt.c - VideoToolbox HEVC behind the n265 calls (docs/hardware-mode.md).
 *
 * A compression session that requires the hardware encoder. Each picture
 * is copied into a buffer from the session's pool, since the session holds
 * frames past the call and the caller reuses its planes. The output
 * callback runs on the session's thread and turns each length-prefixed
 * sample into Annex B on a mutex-guarded queue, which encode drains in
 * decode order. VPS, SPS and PPS come from the format description and go
 * out ahead of the first access unit.
 * Copyright (c) 2026, the yah265 authors. GPL-2.0-or-later; see LICENSE.
 */

#include <VideoToolbox/VideoToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw/vt.h"

#define VT_MAX_NALS 256
#define VT_MAX_AUS  64
#define VT_MAX_PS   8

struct vt_out {                 /* one sample's NALs, Annex B */
    uint8_t *data;
    size_t size;
    int key, display_idx;
    struct vt_out *next;
};

struct n265_hw {
    VTCompressionSessionRef sess;
    n265_hw_cfg cfg;
    int frames_in, frames_out;
    int err;                    /* first callback error, sticky */
    int hdr_sent;
    /* the parameter sets, Annex B, in the order the description lists,
     * then our SEI; hdr_* counts the parameter sets alone */
    uint8_t *ps;
    size_t ps_len, hdr_len;
    int ps_n, hdr_n, ps_type[VT_MAX_PS];
    size_t ps_size[VT_MAX_PS];
    int probing;
    pthread_mutex_t mx;
    pthread_cond_t cv;
    struct vt_out *head, *tail;
    uint8_t *out;
    size_t out_cap;
    n265_nal nal[VT_MAX_NALS];
    n265_au au[VT_MAX_AUS];
    int n_au;
    int reorder;
    /* the HDR10 SEI, which the hardware doesn't write: out beside the
     * parameter sets */
    uint8_t sei[64];
    size_t sei_len;
    char name[64];
};

static void set_err(struct n265_hw *h, int e)
{
    pthread_mutex_lock(&h->mx);
    if (!h->err)
        h->err = e;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mx);
}

/* Annex B parameter sets out of the description, once; returns the NAL
 * length size, or -1 */
static int take_parameter_sets(struct n265_hw *h, CMFormatDescriptionRef fd)
{
    size_t cnt = 0;
    int hlen = 4;
    const uint8_t *p = NULL;
    size_t n = 0;
    if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, 0, &p, &n,
                                                           &cnt, &hlen)
        != noErr)
        return -1;
    if (h->ps)
        return hlen;
    if (cnt > VT_MAX_PS - 1)
        cnt = VT_MAX_PS - 1;
    size_t total = 0;
    for (size_t i = 0; i < cnt; i++) {
        if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, i, &p, &n,
                                                               NULL, NULL)
            != noErr)
            return -1;
        total += 4 + n;
    }
    uint8_t *buf = malloc(total + h->sei_len);
    if (!buf)
        return -1;
    size_t w = 0;
    for (size_t i = 0; i < cnt; i++) {
        CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, i, &p, &n,
                                                           NULL, NULL);
        buf[w] = 0; buf[w + 1] = 0; buf[w + 2] = 0; buf[w + 3] = 1;
        memcpy(buf + w + 4, p, n);
        h->ps_type[i] = n ? (p[0] >> 1) & 0x3f : 0;
        h->ps_size[i] = 4 + n;
        w += 4 + n;
    }
    h->hdr_n = (int)cnt;
    h->hdr_len = total;
    if (h->sei_len) {
        memcpy(buf + w, h->sei, h->sei_len);
        h->ps_type[cnt] = N265_NAL_PREFIX_SEI;
        h->ps_size[cnt] = h->sei_len;
        cnt++;
        total += h->sei_len;
    }
    h->ps_n = (int)cnt;
    h->ps_len = total;
    h->ps = buf;
    return hlen;
}

static void vt_output(void *ref, void *src_ref, OSStatus status,
                      VTEncodeInfoFlags flags, CMSampleBufferRef sb)
{
    struct n265_hw *h = ref;
    if (status != noErr || (!sb && !(flags & kVTEncodeInfo_FrameDropped))) {
        set_err(h, status ? (int)status : -1);
        return;
    }
    int hlen = 4;
    if (sb) {
        CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
        int r = fd ? take_parameter_sets(h, fd) : -1;
        if (r > 0)
            hlen = r;
    }
    if (!src_ref) {             /* the header probe's frame */
        pthread_mutex_lock(&h->mx);
        h->probing = 0;
        pthread_cond_broadcast(&h->cv);
        pthread_mutex_unlock(&h->mx);
        return;
    }
    if (flags & kVTEncodeInfo_FrameDropped) {
        pthread_mutex_lock(&h->mx);
        h->frames_out++;
        pthread_cond_broadcast(&h->cv);
        pthread_mutex_unlock(&h->mx);
        return;
    }
    int key = 1;
    CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (att && CFArrayGetCount(att) > 0) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(att, 0);
        if (d && CFDictionaryGetValue(d, kCMSampleAttachmentKey_NotSync))
            key = 0;
    }
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    size_t total = 0;
    char *base = NULL;
    if (!bb || CMBlockBufferGetDataPointer(bb, 0, NULL, &total, &base)
               != noErr || !base) {
        set_err(h, -2);
        return;
    }
    struct vt_out *o = calloc(1, sizeof *o);
    size_t cap = total + 4 * 16;
    if (!o || !(o->data = malloc(cap))) {
        free(o);
        set_err(h, -3);
        return;
    }
    /* length-prefixed -> Annex B */
    size_t pos = 0, w = 0;
    const uint8_t *s = (const uint8_t *)base;
    while (pos + (size_t)hlen <= total) {
        uint32_t len = 0;
        for (int i = 0; i < hlen; i++)
            len = (len << 8) | s[pos + i];
        pos += (size_t)hlen;
        if (len == 0 || pos + len > total) {
            static _Atomic int said;
            if (!atomic_exchange(&said, 1))
                fprintf(stderr, "y265: hardware sample malformed (NAL "
                        "length %u at %zu of %zu); truncated\n", len, pos,
                        total);
            break;
        }
        if (w + 4 + len > cap) {
            cap = w + 4 + len + 4 * 16;
            uint8_t *nb = realloc(o->data, cap);
            if (!nb)
                break;
            o->data = nb;
        }
        o->data[w] = 0; o->data[w + 1] = 0; o->data[w + 2] = 0;
        o->data[w + 3] = 1;
        memcpy(o->data + w + 4, s + pos, len);
        w += 4 + len;
        pos += len;
    }
    o->size = w;
    o->key = key;
    o->display_idx = (int)((intptr_t)src_ref - 1);
    pthread_mutex_lock(&h->mx);
    h->frames_out++;
    if (h->tail)
        h->tail->next = o;
    else
        h->head = o;
    h->tail = o;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mx);
}

static int set_num(VTCompressionSessionRef s, CFStringRef key, CFNumberType t,
                   const void *v)
{
    CFNumberRef n = CFNumberCreate(NULL, t, v);
    if (!n)
        return -1;
    OSStatus st = VTSessionSetProperty(s, key, n);
    CFRelease(n);
    return st == noErr ? 0 : -1;
}

static int set_i32(VTCompressionSessionRef s, CFStringRef key, int v)
{
    return set_num(s, key, kCFNumberIntType, &v);
}

static int set_f64(VTCompressionSessionRef s, CFStringRef key, double v)
{
    return set_num(s, key, kCFNumberDoubleType, &v);
}

/* --crf onto the hardware's 0..1 quality key, rate-matched: each point is
 * the quality at which the hardware spends what our medium spends at that
 * CRF, the median over eight corpus clips at 120 frames (spread about
 * +-0.05, wider on park_joy and sintel). Linear between points, the same
 * slope outside 20..36. */
static double crf_to_quality(double crf)
{
    static const double t[][2] = {
        { 8, 0.87 }, { 12, 0.79 }, { 16, 0.71 }, { 20, 0.63 },
        { 24, 0.54 }, { 28, 0.43 }, { 32, 0.31 }, { 36, 0.205 },
        { 40, 0.12 }, { 44, 0.06 }, { 48, 0.03 }, { 51, 0.02 },
    };
    const int n = (int)(sizeof t / sizeof t[0]);
    if (crf <= t[0][0])
        return t[0][1];
    for (int i = 1; i < n; i++)
        if (crf <= t[i][0]) {
            double f = (crf - t[i - 1][0]) / (t[i][0] - t[i - 1][0]);
            return t[i - 1][1] + f * (t[i][1] - t[i - 1][1]);
        }
    return t[n - 1][1];
}

static OSType pixel_format(const n265_hw_cfg *c)
{
    if (c->depth > 8)
        return c->full_range == 1
                   ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
                   : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
    return kCVPixelFormatType_420YpCbCr8Planar;
}

/* H.273 codes onto VideoToolbox's names; NULL is "not signalled" */
static CFStringRef prim_name(int v)
{
    switch (v) {
    case 1:  return kCVImageBufferColorPrimaries_ITU_R_709_2;
    case 5:  return kCVImageBufferColorPrimaries_EBU_3213;
    case 6:  return kCVImageBufferColorPrimaries_SMPTE_C;
    case 9:  return kCVImageBufferColorPrimaries_ITU_R_2020;
    case 12: return kCVImageBufferColorPrimaries_P3_D65;
    default: return NULL;
    }
}

static CFStringRef transfer_name(int v)
{
    switch (v) {
    case 1: case 6: return kCVImageBufferTransferFunction_ITU_R_709_2;
    case 7:  return kCVImageBufferTransferFunction_SMPTE_240M_1995;
    case 14: case 15: return kCVImageBufferTransferFunction_ITU_R_2020;
    case 16: return kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ;
    case 18: return kCVImageBufferTransferFunction_ITU_R_2100_HLG;
    default: return NULL;
    }
}

static CFStringRef matrix_name(int v)
{
    switch (v) {
    case 1:  return kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    case 5: case 6: return kCVImageBufferYCbCrMatrix_ITU_R_601_4;
    case 7:  return kCVImageBufferYCbCrMatrix_SMPTE_240M_1995;
    case 9:  return kCVImageBufferYCbCrMatrix_ITU_R_2020;
    default: return NULL;
    }
}

static void put16(uint8_t *b, unsigned v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static void put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);  b[3] = (uint8_t)v;
}

/* a hardware session with the cfg's properties; NULL and a reason */
static VTCompressionSessionRef session_new(struct n265_hw *h, int quiet,
                                           const char **why)
{
    const n265_hw_cfg *c = &h->cfg;
    CFMutableDictionaryRef spec = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(spec,
        kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
        kCFBooleanTrue);
    CFMutableDictionaryRef src = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSType fmt = pixel_format(c);
    CFNumberRef fmtn = CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt);
    CFDictionarySetValue(src, kCVPixelBufferPixelFormatTypeKey, fmtn);
    CFRelease(fmtn);
    VTCompressionSessionRef s = NULL;
    OSStatus st = VTCompressionSessionCreate(NULL, c->width, c->height,
        kCMVideoCodecType_HEVC, spec, src, NULL, vt_output, h, &s);
    CFRelease(spec);
    CFRelease(src);
    if (st != noErr || !s) {
        *why = "no hardware HEVC encoder session";
        return NULL;
    }
    CFBooleanRef hwacc = NULL;
    int ok = 0;
    if (VTSessionCopyProperty(s,
            kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
            NULL, &hwacc) == noErr && hwacc) {
        ok = CFBooleanGetValue(hwacc);
        CFRelease(hwacc);
    }
    if (!ok) {
        *why = "VideoToolbox offered only its software encoder; refused";
        goto fail;
    }

    VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime,
                         c->realtime ? kCFBooleanTrue : kCFBooleanFalse);
    if (VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel,
            c->depth > 8 ? kVTProfileLevel_HEVC_Main10_AutoLevel
                         : kVTProfileLevel_HEVC_Main_AutoLevel) != noErr) {
        *why = c->depth > 8 ? "the hardware refused Main10"
                            : "the hardware refused Main";
        goto fail;
    }
    VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering,
                         c->bframes > 0 ? kCFBooleanTrue : kCFBooleanFalse);
    if (c->keyint > 0)
        set_i32(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, c->keyint);
    set_f64(s, kVTCompressionPropertyKey_ExpectedFrameRate,
            (double)c->fps_num / c->fps_den);

    if (c->bitrate > 0.0) {
        set_i32(s, kVTCompressionPropertyKey_AverageBitRate,
                (int)(c->bitrate * 1000.0));
    } else if (c->crf_on) {
        set_f64(s, kVTCompressionPropertyKey_Quality, crf_to_quality(c->crf));
    } else {
        int lo = set_i32(s, CFSTR("MinAllowedFrameQP"), c->qp);
        int hi = set_i32(s, CFSTR("MaxAllowedFrameQP"), c->qp);
        if (lo || hi) {
            if (!quiet)
                fprintf(stderr, "y265: hw: the hardware won't pin the frame "
                        "QP; --qp %d runs as the quality of --crf %d\n",
                        c->qp, c->qp);
            set_f64(s, kVTCompressionPropertyKey_Quality,
                    crf_to_quality(c->qp));
        }
    }
    if (c->vbv_maxrate > 0.0 && c->vbv_bufsize > 0.0) {
        double win = c->vbv_bufsize / c->vbv_maxrate;
        double bytes = c->vbv_maxrate * 1000.0 / 8.0 * win;
        CFNumberRef b = CFNumberCreate(NULL, kCFNumberDoubleType, &bytes);
        CFNumberRef wn = CFNumberCreate(NULL, kCFNumberDoubleType, &win);
        const void *vals[2] = { b, wn };
        CFArrayRef arr = CFArrayCreate(NULL, vals, 2, &kCFTypeArrayCallBacks);
        if (arr) {
            if (VTSessionSetProperty(s,
                    kVTCompressionPropertyKey_DataRateLimits, arr) != noErr &&
                !quiet)
                fprintf(stderr, "y265: hw: the hardware refused the data-rate "
                        "limit; the VBV is not applied\n");
            CFRelease(arr);
        }
        if (b)
            CFRelease(b);
        if (wn)
            CFRelease(wn);
    }

    /* the VUI */
    {
        struct { int v; CFStringRef key; CFStringRef name; const char *flag; } sig[3] = {
            { c->colorprim, kVTCompressionPropertyKey_ColorPrimaries,
              prim_name(c->colorprim), "colorprim" },
            { c->transfer, kVTCompressionPropertyKey_TransferFunction,
              transfer_name(c->transfer), "transfer" },
            { c->colormatrix, kVTCompressionPropertyKey_YCbCrMatrix,
              matrix_name(c->colormatrix), "colormatrix" },
        };
        for (int i = 0; i < 3; i++) {
            if (sig[i].v < 0 || sig[i].v == 2)
                continue;
            if (!sig[i].name ||
                VTSessionSetProperty(s, sig[i].key, sig[i].name) != noErr) {
                if (!quiet)
                    fprintf(stderr, "y265: hw: --%s %d has no hardware "
                            "spelling; not signalled\n", sig[i].flag,
                            sig[i].v);
            }
        }
    }
    if (c->sar_w > 0 && c->sar_h > 0 && c->sar_w != c->sar_h) {
        CFNumberRef hn = CFNumberCreate(NULL, kCFNumberIntType, &c->sar_w);
        CFNumberRef vn = CFNumberCreate(NULL, kCFNumberIntType, &c->sar_h);
        const void *k[2] = {
            kCVImageBufferPixelAspectRatioHorizontalSpacingKey,
            kCVImageBufferPixelAspectRatioVerticalSpacingKey,
        };
        const void *v[2] = { hn, vn };
        CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (d) {
            VTSessionSetProperty(s, kVTCompressionPropertyKey_PixelAspectRatio,
                                 d);
            CFRelease(d);
        }
        CFRelease(hn);
        CFRelease(vn);
    }
    if (VTCompressionSessionPrepareToEncodeFrames(s) != noErr) {
        *why = "the hardware session refused to prepare";
        goto fail;
    }
    return s;
fail:
    VTCompressionSessionInvalidate(s);
    CFRelease(s);
    return NULL;
}

struct n265_hw *n265_hw_open(const n265_hw_cfg *c, char *why, size_t whylen)
{
    const char *msg = NULL;
    if (c->csp != N265_CSP_I420)
        msg = "the hardware encoder takes 4:2:0 only";
    else if (c->depth != 8 && c->depth != 10)
        msg = "the hardware encoder takes 8 or 10 bits";
    if (msg) {
        if (why && whylen)
            snprintf(why, whylen, "%s", msg);
        return NULL;
    }
    struct n265_hw *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    pthread_mutex_init(&h->mx, NULL);
    pthread_cond_init(&h->cv, NULL);
    h->cfg = *c;
    if (h->cfg.fps_num <= 0 || h->cfg.fps_den <= 0) {
        h->cfg.fps_num = 25;
        h->cfg.fps_den = 1;
    }
    h->reorder = -1;
    if (c->have_md || c->have_cll) {
        uint8_t r[48];
        size_t n = 0;
        if (c->have_md) {
            r[n++] = 137;
            r[n++] = 24;
            for (int i = 0; i < 3; i++) {
                put16(r + n, c->md_prim[i][0]);
                put16(r + n + 2, c->md_prim[i][1]);
                n += 4;
            }
            put16(r + n, c->md_wp[0]);
            put16(r + n + 2, c->md_wp[1]);
            put32(r + n + 4, c->md_max);
            put32(r + n + 8, c->md_min);
            n += 12;
        }
        if (c->have_cll) {
            r[n++] = 144;
            r[n++] = 4;
            put16(r + n, (unsigned)c->maxcll);
            put16(r + n + 2, (unsigned)c->maxfall);
            n += 4;
        }
        r[n++] = 0x80;
        uint8_t *w = h->sei;
        static const uint8_t head[6] = { 0, 0, 0, 1, 0x4e, 0x01 };
        memcpy(w, head, sizeof head);
        size_t k = sizeof head;
        for (size_t i = 0, zeros = 0; i < n; i++) {
            if (zeros >= 2 && r[i] <= 3) {
                w[k++] = 3;
                zeros = 0;
            }
            w[k++] = r[i];
            zeros = r[i] ? 0 : zeros + 1;
        }
        h->sei_len = k;
    }
    h->sess = session_new(h, 0, &msg);
    if (!h->sess) {
        if (why && whylen)
            snprintf(why, whylen, "%s", msg);
        n265_hw_close(h);
        return NULL;
    }
    snprintf(h->name, sizeof h->name, "VideoToolbox HEVC %s (hardware)",
             c->depth > 8 ? "Main10" : "Main");
    return h;
}

const char *n265_hw_name(const struct n265_hw *h)
{
    return h ? h->name : "none";
}

static int fill_buffer(const struct n265_hw *h, CVPixelBufferRef pb,
                       const n265_picture *pic)
{
    const n265_hw_cfg *c = &h->cfg;
    if (CVPixelBufferLockBaseAddress(pb, 0) != kCVReturnSuccess)
        return -1;
    int cw = (c->width + 1) / 2, ch = (c->height + 1) / 2;
    if (c->depth == 8) {
        for (int p = 0; p < 3; p++) {
            uint8_t *dst = CVPixelBufferGetBaseAddressOfPlane(pb, (size_t)p);
            size_t ds = CVPixelBufferGetBytesPerRowOfPlane(pb, (size_t)p);
            int w = p ? cw : c->width, hh = p ? ch : c->height;
            const uint8_t *s = pic ? pic->plane[p] : NULL;
            for (int y = 0; y < hh; y++) {
                if (s)
                    memcpy(dst + (size_t)y * ds,
                           s + (size_t)y * pic->stride[p], (size_t)w);
                else
                    memset(dst + (size_t)y * ds, 128, (size_t)w);
            }
        }
    } else {
        /* samples sit in the top bits; chroma interleaved */
        uint8_t *dy = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        size_t sy = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
        uint8_t *dc = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        size_t sc = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
        for (int y = 0; y < c->height; y++) {
            uint16_t *d = (uint16_t *)(dy + (size_t)y * sy);
            const uint16_t *s = pic ? (const uint16_t *)pic->plane[0] +
                                      (size_t)y * pic->stride[0] : NULL;
            for (int x = 0; x < c->width; x++)
                d[x] = (uint16_t)((s ? s[x] : 512) << 6);
        }
        for (int y = 0; y < ch; y++) {
            uint16_t *d = (uint16_t *)(dc + (size_t)y * sc);
            const uint16_t *u = pic ? (const uint16_t *)pic->plane[1] +
                                      (size_t)y * pic->stride[1] : NULL;
            const uint16_t *v = pic ? (const uint16_t *)pic->plane[2] +
                                      (size_t)y * pic->stride[2] : NULL;
            for (int x = 0; x < cw; x++) {
                d[2 * x] = (uint16_t)((u ? u[x] : 512) << 6);
                d[2 * x + 1] = (uint16_t)((v ? v[x] : 512) << 6);
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return 0;
}

static CVPixelBufferRef new_buffer(const struct n265_hw *h,
                                   VTCompressionSessionRef s)
{
    const n265_hw_cfg *c = &h->cfg;
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(s);
    CVPixelBufferRef pb = NULL;
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb)
                 != kCVReturnSuccess || !pb) {
        pb = NULL;
        if (CVPixelBufferCreate(NULL, (size_t)c->width, (size_t)c->height,
                                pixel_format(c), NULL, &pb)
            != kCVReturnSuccess)
            return NULL;
    }
    return pb;
}

/* The parameter sets before any picture: one grey frame through a second
 * session with the same properties. */
static int probe_headers(struct n265_hw *h)
{
    if (h->ps)
        return 0;
    const char *why = NULL;
    VTCompressionSessionRef s = session_new(h, 1, &why);
    if (!s)
        return -1;
    CVPixelBufferRef pb = new_buffer(h, s);
    int rc = -1;
    if (pb && fill_buffer(h, pb, NULL) == 0) {
        pthread_mutex_lock(&h->mx);
        h->probing = 1;
        pthread_mutex_unlock(&h->mx);
        if (VTCompressionSessionEncodeFrame(s, pb, CMTimeMake(0, 1),
                CMTimeMake(h->cfg.fps_den, h->cfg.fps_num), NULL, NULL,
                NULL) == noErr) {
            VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
            pthread_mutex_lock(&h->mx);
            while (h->probing && !h->err)
                pthread_cond_wait(&h->cv, &h->mx);
            pthread_mutex_unlock(&h->mx);
            rc = h->ps ? 0 : -1;
        }
    }
    if (pb)
        CVPixelBufferRelease(pb);
    VTCompressionSessionInvalidate(s);
    CFRelease(s);
    return rc;
}

int n265_hw_headers(struct n265_hw *h, n265_nal **nal, int *nnal)
{
    if (!h || !nal || !nnal)
        return -1;
    *nal = h->nal;
    *nnal = 0;
    h->n_au = 0;
    if (probe_headers(h) < 0)
        return -1;
    if (h->hdr_len > h->out_cap) {
        uint8_t *o = realloc(h->out, h->hdr_len);
        if (!o)
            return -1;
        h->out = o;
        h->out_cap = h->hdr_len;
    }
    memcpy(h->out, h->ps, h->hdr_len);
    size_t off = 0;
    for (int i = 0; i < h->hdr_n; i++) {
        h->nal[i].type = h->ps_type[i];
        h->nal[i].size = (int)h->ps_size[i];
        h->nal[i].payload = h->out + off;
        off += h->ps_size[i];
    }
    *nnal = h->hdr_n;
    return (int)h->hdr_len;
}

/* sps_max_num_reorder_pics of the highest sub-layer */
typedef struct { const uint8_t *b; size_t n, pos; } rbsp_rd;

static int rd_bit(rbsp_rd *r)
{
    if (r->pos >= r->n * 8)
        return 0;
    int v = (r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1;
    r->pos++;
    return v;
}

static uint32_t rd_u(rbsp_rd *r, int n)
{
    uint32_t v = 0;
    while (n--)
        v = (v << 1) | (uint32_t)rd_bit(r);
    return v;
}

static uint32_t rd_ue(rbsp_rd *r)
{
    int z = 0;
    while (!rd_bit(r) && z < 32)
        z++;
    return z ? ((1u << z) - 1) + rd_u(r, z) : 0;
}

static int sps_reorder(const uint8_t *nal, size_t n)
{
    uint8_t *b = malloc(n);
    if (!b)
        return -1;
    size_t w = 0;
    for (size_t i = 2, zeros = 0; i < n; i++) {   /* past the NAL header */
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] ? 0 : zeros + 1;
        b[w++] = nal[i];
    }
    rbsp_rd r = { b, w, 0 };
    rd_u(&r, 4);
    int msl = (int)rd_u(&r, 3);
    rd_u(&r, 1);
    rd_u(&r, 96);                                 /* general profile and level */
    int pp[8] = { 0 }, lp[8] = { 0 };
    for (int i = 0; i < msl; i++) {
        pp[i] = rd_bit(&r);
        lp[i] = rd_bit(&r);
    }
    if (msl > 0)
        for (int i = msl; i < 8; i++)
            rd_u(&r, 2);
    for (int i = 0; i < msl; i++) {
        if (pp[i])
            rd_u(&r, 88);
        if (lp[i])
            rd_u(&r, 8);
    }
    rd_ue(&r);
    if (rd_ue(&r) == 3)
        rd_bit(&r);
    rd_ue(&r);
    rd_ue(&r);
    if (rd_bit(&r))
        for (int i = 0; i < 4; i++)
            rd_ue(&r);
    rd_ue(&r);
    rd_ue(&r);
    rd_ue(&r);
    int all = rd_bit(&r), reorder = 0;
    for (int i = all ? 0 : msl; i <= msl; i++) {
        rd_ue(&r);
        reorder = (int)rd_ue(&r);
        rd_ue(&r);
    }
    free(b);
    return reorder;
}

int n265_hw_reorder_depth(const struct n265_hw *hc)
{
    struct n265_hw *h = (struct n265_hw *)hc;
    if (!h)
        return -1;
    if (h->reorder >= 0)
        return h->reorder;
    if (probe_headers(h) < 0)
        return h->cfg.bframes > 0 ? 2 : 0;
    size_t off = 0;
    for (int i = 0; i < h->ps_n; i++) {
        if (h->ps_type[i] == N265_NAL_SPS)
            h->reorder = sps_reorder(h->ps + off + 4, h->ps_size[i] - 4);
        off += h->ps_size[i];
    }
    return h->reorder >= 0 ? h->reorder : 0;
}

int n265_hw_access_units(const struct n265_hw *h, const n265_au **au)
{
    if (!h || !au)
        return -1;
    *au = h->au;
    return h->n_au;
}

/* every queued sample into the caller's buffer, split back into NALs, one
 * access unit each */
static int drain(struct n265_hw *h, n265_nal **nal, int *nnal, int wait_for)
{
    pthread_mutex_lock(&h->mx);
    while (wait_for && !h->err && h->frames_out < wait_for)
        pthread_cond_wait(&h->cv, &h->mx);
    struct vt_out *o = h->head;
    int err = h->err;
    /* stop at what the arrays hold; the rest waits for the next call */
    struct vt_out *last = NULL;
    int ns = 0, nn = 0;
    for (struct vt_out *q = o; q && ns < VT_MAX_AUS; q = q->next) {
        int k = h->ps_n;
        for (size_t i = 0; i + 4 <= q->size; i++)
            k += q->data[i] == 0 && q->data[i + 1] == 0 &&
                 q->data[i + 2] == 0 && q->data[i + 3] == 1;
        if (nn + k > VT_MAX_NALS)
            break;
        nn += k;
        ns++;
        last = q;
    }
    if (last) {
        h->head = last->next;
        if (!h->head)
            h->tail = NULL;
        last->next = NULL;
    } else {
        o = NULL;
    }
    pthread_mutex_unlock(&h->mx);

    size_t need = 0;
    for (struct vt_out *q = o; q; q = q->next) {
        need += q->size;
        if (!h->hdr_sent || (q->key && h->cfg.repeat_headers))
            need += h->ps_len;
    }
    if (need > h->out_cap) {
        uint8_t *nb = realloc(h->out, need);
        if (!nb) {
            err = -3;
            need = 0;
        } else {
            h->out = nb;
            h->out_cap = need;
        }
    }
    size_t w = 0;
    int n = 0;
    h->n_au = 0;
    while (o) {
        struct vt_out *nx = o->next;
        if (need) {
            n265_au *a = &h->au[h->n_au++];
            a->display_idx = o->display_idx;
            a->first_nal = n;
            if (h->ps && (!h->hdr_sent || (o->key && h->cfg.repeat_headers))) {
                memcpy(h->out + w, h->ps, h->ps_len);
                for (int i = 0; i < h->ps_n; i++) {
                    h->nal[n].type = h->ps_type[i];
                    h->nal[n].size = (int)h->ps_size[i];
                    h->nal[n].payload = h->out + w;
                    w += h->ps_size[i];
                    n++;
                }
                h->hdr_sent = 1;
            }
            memcpy(h->out + w, o->data, o->size);
            size_t s = w, end = w + o->size;
            for (size_t i = w + 4; i <= end; i++) {
                int at_end = i + 4 > end;
                if (at_end || (h->out[i] == 0 && h->out[i + 1] == 0 &&
                               h->out[i + 2] == 0 && h->out[i + 3] == 1)) {
                    if (at_end)
                        i = end;
                    h->nal[n].type = (h->out[s + 4] >> 1) & 0x3f;
                    h->nal[n].size = (int)(i - s);
                    h->nal[n].payload = h->out + s;
                    n++;
                    s = i;
                    if (at_end)
                        break;
                }
            }
            w = end;
            a->nnal = n - a->first_nal;
        }
        free(o->data);
        free(o);
        o = nx;
    }
    *nal = h->nal;
    *nnal = n;
    if (err)
        return -1;
    return (int)w;
}

int n265_hw_encode(struct n265_hw *h, n265_nal **nal, int *nnal,
                   const n265_picture *pic, int force_key)
{
    if (!h || !nal || !nnal)
        return -1;
    if (!pic) {
        if (h->frames_in > h->frames_out || h->head) {
            VTCompressionSessionCompleteFrames(h->sess, kCMTimeInvalid);
            return drain(h, nal, nnal, h->frames_in);
        }
        *nal = h->nal;
        *nnal = 0;
        h->n_au = 0;
        return 0;
    }
    CVPixelBufferRef pb = new_buffer(h, h->sess);
    if (!pb)
        return -1;
    if (fill_buffer(h, pb, pic) < 0) {
        CVPixelBufferRelease(pb);
        return -1;
    }
    /* display order counted here: a caller's pts need not be frames */
    CMTime pts = CMTimeMake((int64_t)h->frames_in * h->cfg.fps_den,
                            h->cfg.fps_num);
    CMTime dur = CMTimeMake(h->cfg.fps_den, h->cfg.fps_num);
    CFDictionaryRef fopts = NULL;
    if (force_key) {
        const void *k[1] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
        const void *v[1] = { kCFBooleanTrue };
        fopts = CFDictionaryCreate(NULL, k, v, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    OSStatus st = VTCompressionSessionEncodeFrame(h->sess, pb, pts, dur, fopts,
        (void *)(intptr_t)(h->frames_in + 1), NULL);
    if (fopts)
        CFRelease(fopts);
    CVPixelBufferRelease(pb);
    if (st != noErr)
        return -1;
    h->frames_in++;
    return drain(h, nal, nnal, 0);
}

void n265_hw_close(struct n265_hw *h)
{
    if (!h)
        return;
    if (h->sess) {
        VTCompressionSessionInvalidate(h->sess);
        CFRelease(h->sess);
    }
    struct vt_out *o = h->head;
    while (o) {
        struct vt_out *nx = o->next;
        free(o->data);
        free(o);
        o = nx;
    }
    free(h->ps);
    free(h->out);
    pthread_mutex_destroy(&h->mx);
    pthread_cond_destroy(&h->cv);
    free(h);
}
