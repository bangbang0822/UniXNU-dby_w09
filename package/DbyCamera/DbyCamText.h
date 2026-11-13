/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 可移植源码的小工具：内核（没有 C 库）和主机测试都能编。文本拼接用来生成单行日志 "[cam] ..."，绝不写出缓冲区，
 * 也总是留下结尾 0。取自母本 PaganiCamera 的 CamUtil.h（它取自 PaganiBt BtUtil.h），名字改成 dbcam_ 前缀
 * （本仓几个驱动在同一个归档里，前缀必须各不相同：手电筒是 dbt_，插件侧是 dbc_）。 */
#ifndef DBY_CAM_TEXT_H
#define DBY_CAM_TEXT_H
#include <stddef.h>
#include <stdint.h>

static inline void dbcam_copy(void *to, const void *from, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    const uint8_t *s = (const uint8_t *)from;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
}
static inline void dbcam_zero(void *to, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    for (size_t i = 0; i < n; ++i) d[i] = 0;
}
static inline size_t dbcam_strlen(const char *s, size_t max)
{
    size_t n = 0;
    while (s && n < max && s[n]) ++n;
    return n;
}

typedef struct {
    char *buf;
    size_t size, used;
} DbcamText;

static inline void dbcam_text_start(DbcamText *t, char *buf, size_t size)
{
    t->buf = buf;
    t->size = size;
    t->used = 0;
    if (size) buf[0] = 0;
}
static inline void dbcam_text_char(DbcamText *t, char c)
{
    if (t->used + 1 < t->size) {
        t->buf[t->used++] = c;
        t->buf[t->used] = 0;
    }
}
static inline void dbcam_text_str(DbcamText *t, const char *s)
{
    for (size_t i = 0; s && s[i]; ++i) dbcam_text_char(t, s[i]);
}
static inline void dbcam_text_udec(DbcamText *t, uint64_t v)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof digits);
    while (n) dbcam_text_char(t, digits[--n]);
}
static inline void dbcam_text_dec(DbcamText *t, int64_t v)
{
    if (v < 0) {
        dbcam_text_char(t, '-');
        dbcam_text_udec(t, (uint64_t)0 - (uint64_t)v);
    } else {
        dbcam_text_udec(t, (uint64_t)v);
    }
}
/* 带 0x 前缀，不补零。 */
static inline void dbcam_text_hex(DbcamText *t, uint64_t v)
{
    char digits[16];
    unsigned n = 0;
    do {
        digits[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v && n < sizeof digits);
    dbcam_text_str(t, "0x");
    while (n) dbcam_text_char(t, digits[--n]);
}
/* " key=" 加值。 */
static inline void dbcam_text_kd(DbcamText *t, const char *key, int64_t v)
{
    dbcam_text_char(t, ' ');
    dbcam_text_str(t, key);
    dbcam_text_char(t, '=');
    dbcam_text_dec(t, v);
}
static inline void dbcam_text_kx(DbcamText *t, const char *key, uint64_t v)
{
    dbcam_text_char(t, ' ');
    dbcam_text_str(t, key);
    dbcam_text_char(t, '=');
    dbcam_text_hex(t, v);
}
#endif
