/*
 * linker.ld - カーネルをメモリのどこに、どの順番で並べるかの指示書
 *
 * stage2.asm は 0x8000 番地にジャンプするので、その番地に entry.asm の _start が
 * 来るように並べる。
 */
ENTRY(_start)

SECTIONS
{
    . = 0x8000;                 /* ここから並べ始める */

    .text : {                   /* 機械語のプログラム */
        *(.text.entry)          /* entry.asm の _start を必ず先頭に */
        *(.text*)
    }
    .rodata : { *(.rodata*) }   /* 書き換えないデータ(文字列など) */
    .data   : { *(.data*) }     /* 初期値のあるグローバル変数 */

    .bss : {                    /* 初期値のないグローバル変数(entry.asm が0で埋める) */
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        __bss_end = .;
    }

    /* OS には不要な、コンパイラが付け足す情報を捨てる */
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
