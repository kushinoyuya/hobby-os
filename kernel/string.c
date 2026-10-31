/*
 * string.c - メモリを埋める・写す
 *
 * memset / memcpy を C のループで書くと、gcc が「これは memset だ」と見抜いて
 * memset の呼び出しに置き換え、自分自身を呼び続けてしまうことがある。
 * そこで x86 の rep stosb / rep movsb 命令(ecx 回くり返す)を直接使う。
 */
#include "string.h"

void *memset(void *dst, int value, size_t n)
{
    void *d = dst;
    /* edi の番地から ecx バイトを al の値で埋める */
    __asm__ volatile ("rep stosb"
                      : "+D"(d), "+c"(n)
                      : "a"(value)
                      : "memory");
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    void *d = dst;
    /* esi の番地から edi の番地へ ecx バイト写す */
    __asm__ volatile ("rep movsb"
                      : "+D"(d), "+S"(src), "+c"(n)
                      :
                      : "memory");
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
