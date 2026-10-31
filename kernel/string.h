/*
 * string.h - メモリを埋める・写す
 *
 * 標準ライブラリがないので自分で用意する。gcc は構造体のコピーなどで
 * memset / memcpy を勝手に呼ぶことがあるので、この名前で必ず置いておく。
 */
#ifndef STRING_H
#define STRING_H

#include <stddef.h>

void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int   strcmp(const char *a, const char *b);

#endif
