; boot.asm - ブートセクタ(ステージ1)
;
; PCの電源が入ると、BIOSはディスクの先頭512バイト(ブートセクタ)を
; メモリの 0x7C00 番地に読み込み、そこから実行を始める。
; 512バイトでは足りないので、このプログラムの仕事は
; 「ディスクの続き(ステージ2)をメモリに読み込んで、そこへジャンプする」ことだけ。

[bits 16]               ; 起動直後のCPUは16ビットモード(リアルモード)で動いている
[org 0x7c00]            ; このプログラムは 0x7C00 番地に置かれる前提でアドレスを計算する

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 32   ; ステージ2の大きさ(セクタ数)。Makefile から -D で渡される
%endif

STAGE2_ADDR equ 0x7e00  ; ステージ2を置く番地(ブートセクタの直後)

start:
    ; セグメントレジスタとスタックを初期化する
    cli                 ; 初期化中は割り込みを止める
    xor ax, ax          ; ax = 0
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00      ; スタックはこのプログラムの直前(下位アドレス側)に伸びていく
    sti

    mov [boot_drive], dl    ; BIOSは「起動したディスクの番号」を dl に入れて渡してくれる

    mov si, msg_loading
    call print

    ; BIOSのディスク読み込み機能(int 0x13, ah=0x42)でステージ2を読み込む。
    ; 読み込む場所や量は、下の dap(ディスクアドレスパケット)に書いておく。
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error       ; 失敗するとキャリーフラグが立つ

    mov dl, [boot_drive]    ; ステージ2でもディスク番号を使えるように dl で渡す
    jmp 0x0000:STAGE2_ADDR  ; ステージ2へジャンプ(ここから先はもう戻ってこない)

disk_error:
    mov si, msg_disk_error
    call print
.hang:
    hlt
    jmp .hang

%include "print16.inc"

boot_drive:
    db 0

; ディスクアドレスパケット: int 0x13 ah=0x42 に「何をどこへ読むか」を伝える16バイトの表
dap:
    db 0x10             ; この表の大きさ(16バイト)
    db 0                ; 予約(常に0)
    dw STAGE2_SECTORS   ; 読み込むセクタ数
    dw STAGE2_ADDR      ; 読み込み先の番地(オフセット)
    dw 0                ; 読み込み先の番地(セグメント)
    dq 1                ; ディスクの何セクタ目から読むか(0がブートセクタなので1から)

msg_loading:
    db "Stage 1: loading stage 2...", 0x0d, 0x0a, 0
msg_disk_error:
    db "Disk read error!", 0x0d, 0x0a, 0

    ; 510バイト目まで0で埋め、最後の2バイトに「ブート可能」の印 0xAA55 を置く
    times 510 - ($ - $$) db 0
    dw 0xaa55
