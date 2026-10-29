; isr.asm - 割り込みの入り口(ISR: Interrupt Service Routine)
;
; 割り込みが起きると、CPU は IDT に登録された番地(ここの isrN)に飛んでくる。
; どの番号の割り込みかを C に伝えるため、番号をスタックに積んでから共通処理へ進み、
; レジスタを全部保存して C の isr_handler() を呼ぶ。終わったら全部戻して iret で元の処理に帰る。
;
; 番号の割り当て:
;   0〜31 : CPU の例外(0で割った、存在しない命令を実行した など)
;   32〜47: 装置からの割り込み(IRQ 0〜15。PIC でこの番号にずらしてある)

[bits 32]

extern isr_handler

; エラーコードを積まない割り込み用: 形をそろえるため、ダミーの 0 を積む
%macro ISR_NOERR 1
isr%1:
    push dword 0            ; ダミーのエラーコード
    push dword %1           ; 割り込み番号
    jmp isr_common
%endmacro

; CPU がエラーコードを自動で積む例外用(8, 10〜14, 17, 21, 29, 30 番)
%macro ISR_ERR 1
isr%1:
    push dword %1           ; 割り込み番号(エラーコードは CPU が積み済み)
    jmp isr_common
%endmacro

section .text

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
%assign i 32                ; 32〜47(IRQ)はエラーコードなし
%rep 16
ISR_NOERR i
%assign i i+1
%endrep

; 全部の割り込みで共通の処理
isr_common:
    pusha                   ; eax, ecx, edx, ebx, esp, ebp, esi, edi を保存
    cld                     ; C のコードは「方向フラグ = 0」を前提にしている
    push esp                ; 引数: 保存したレジスタの場所(struct regs *)
    call isr_handler
    add esp, 4              ; 引数を片付ける
    popa                    ; レジスタを元に戻す
    add esp, 8              ; 割り込み番号とエラーコードを片付ける
    iret                    ; 割り込まれた場所に戻る(eip, cs, eflags を CPU が戻す)

; idt.c が IDT に登録するための、isr0〜isr47 の番地の一覧
section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
    dd isr%+i
%assign i i+1
%endrep
