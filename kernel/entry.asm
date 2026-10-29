; entry.asm - カーネルの入り口
;
; stage2.asm がプロテクトモードに切り替えたあと、ここ(0x8000 番地)にジャンプしてくる。
; C の関数 kmain() を呼ぶための最小限の準備をする。

[bits 32]

section .text.entry         ; リンカスクリプトで、このセクションを一番先頭に置く
global _start
extern kmain                ; kernel.c で定義されている
extern __bss_start          ; linker.ld で定義される番地
extern __bss_end

_start:
    ; .bss(初期値のないグローバル変数の領域)を0で埋める。
    ; C では「初期値のないグローバル変数は0」という約束があるため。
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi            ; ecx = .bss の大きさ(バイト)
    xor eax, eax
    cld
    rep stosb               ; [edi] から ecx バイトを al(=0)で埋める

    call kmain              ; C の世界へ

.hang:                      ; kmain から戻ってきたら止まる
    cli
    hlt
    jmp .hang
