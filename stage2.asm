; stage2.asm - ステージ2
;
; boot.asm(ステージ1)がディスクから読み込んで、0x7E00 番地から実行する。
; 512バイトの制限がないので、ここから先は大きなプログラムを書ける。
; 次の段階では、ここで32ビットモードへの切り替えを行う。

[bits 16]
[org 0x7e00]            ; ステージ1が読み込む番地と合わせる

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 32
%endif

stage2:
    mov si, msg_hello
    call print

    ; ステージ2の「最後のセクタ」に置いた文字列を表示する。
    ; これが表示できれば、全セクタが正しく読み込まれている証拠になる。
    mov si, msg_tail
    call print

.hang:
    hlt
    jmp .hang

%include "print16.inc"

msg_hello:
    db "Stage 2: Hello from beyond the 512-byte boot sector!", 0x0d, 0x0a, 0

    ; ステージ2の末尾まで0で埋め、最後に確認用の文字列を置く。
    ; ステージ2が大きくなりすぎて STAGE2_SECTORS に収まらなくなると、
    ; ここの times が負の数になり、nasm がエラーで知らせてくれる。
    times STAGE2_SECTORS * 512 - (tail_end - msg_tail) - ($ - $$) db 0
msg_tail:
    db "Stage 2: last sector loaded OK.", 0x0d, 0x0a, 0
tail_end:
