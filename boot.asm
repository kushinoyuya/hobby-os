; boot.asm - 画面に "Hello, OS!" と表示するだけのブートセクタ
;
; PCの電源が入ると、BIOSはディスクの先頭512バイト(ブートセクタ)を
; メモリの 0x7C00 番地に読み込み、そこから実行を始める。

[bits 16]               ; 起動直後のCPUは16ビットモード(リアルモード)で動いている
[org 0x7c00]            ; このプログラムは 0x7C00 番地に置かれる前提でアドレスを計算する

start:
    ; セグメントレジスタとスタックを初期化する
    cli                 ; 初期化中は割り込みを止める
    xor ax, ax          ; ax = 0
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00      ; スタックはこのプログラムの直前(下位アドレス側)に伸びていく
    sti

    mov si, message     ; si = 表示する文字列の先頭アドレス
.print:
    lodsb               ; al = [si] を読み込み、si を1つ進める
    test al, al         ; 0(文字列の終わり)なら
    jz .done            ;   ループを抜ける
    mov ah, 0x0e        ; BIOSの「1文字表示」機能
    int 0x10            ; BIOSを呼び出して al の文字を表示
    jmp .print

.done:
    hlt                 ; 何もすることがないのでCPUを休ませる
    jmp .done

message:
    db "Hello, OS!", 0x0d, 0x0a, 0   ; 0x0d 0x0a = 改行、最後の0 = 文字列の終わり

    ; 510バイト目まで0で埋め、最後の2バイトに「ブート可能」の印 0xAA55 を置く
    times 510 - ($ - $$) db 0
    dw 0xaa55
