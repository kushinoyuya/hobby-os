; stage2.asm - ステージ2(512バイト)
;
; boot.asm(ステージ1)がディスクから読み込んで、0x7E00 番地から実行する。
; CPU を16ビットのリアルモードから32ビットのプロテクトモードに切り替え、
; C で書いたカーネル(0x8000 番地に読み込み済み)へジャンプする。

[bits 16]
[org 0x7e00]

KERNEL_ADDR equ 0x8000              ; カーネルの置き場所(このファイルの直後)

CODE_SEG equ gdt_code - gdt_start   ; コード用セグメントの番号(GDT内の位置 = 0x08)
DATA_SEG equ gdt_data - gdt_start   ; データ用セグメントの番号(GDT内の位置 = 0x10)

stage2:
    mov si, msg_real
    call print

    ; A20ラインを有効にする(1MBより上のメモリを正しく使えるようにする)
    in al, 0x92
    or al, 2
    out 0x92, al

    cli                     ; 切り替え中に割り込みが来ると困るので止める
    lgdt [gdt_descriptor]   ; GDTの場所をCPUに教える

    mov eax, cr0
    or eax, 1               ; CR0の最下位ビット(PE)を1にすると
    mov cr0, eax            ;   プロテクトモードになる

    jmp CODE_SEG:protected_mode  ; ファージャンプで CS を新しいセグメントに切り替える

%include "print16.inc"

; ---- GDT(グローバルディスクリプタテーブル) ----
; メモリをどう使うか(どこからどこまで、コードかデータか)をCPUに教える表。
; 今回は「0番地から4GBまで全部」を1つのコード用と1つのデータ用として登録する。
gdt_start:
gdt_null:                   ; 0番目は必ず空にする決まり
    dq 0
gdt_code:                   ; 1番目: コード用
    dw 0xffff               ; 上限(下位16ビット)
    dw 0                    ; 開始番地(0〜15ビット)
    db 0                    ; 開始番地(16〜23ビット)
    db 10011010b            ; 存在する / 特権レベル0 / コード / 実行・読み込み可
    db 11001111b            ; 4KB単位 / 32ビット / 上限(上位4ビット)
    db 0                    ; 開始番地(24〜31ビット)
gdt_data:                   ; 2番目: データ用
    dw 0xffff
    dw 0
    db 0
    db 10010010b            ; 存在する / 特権レベル0 / データ / 読み書き可
    db 11001111b
    db 0
gdt_end:

gdt_descriptor:             ; lgdt に渡す「GDTの大きさと場所」
    dw gdt_end - gdt_start - 1
    dd gdt_start

; ---- ここから32ビットのプロテクトモード ----
[bits 32]
protected_mode:
    mov ax, DATA_SEG        ; データ用のセグメントレジスタを全部切り替える
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000        ; スタックを置き直す

    jmp KERNEL_ADDR         ; C のカーネルへ(kernel/entry.asm の _start)

msg_real:
    db "Stage 2: switching to 32-bit protected mode...", 0x0d, 0x0a, 0

    ; ちょうど512バイトにする。中身が増えて収まらなくなると nasm がエラーを出す
    times 512 - ($ - $$) db 0
