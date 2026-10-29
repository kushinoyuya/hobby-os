; print16.inc - リアルモード用の文字列表示ルーチン(boot.asm と stage2.asm で共用)
;
; 使い方: si に0終端の文字列のアドレスを入れて call print

print:
    pusha               ; 使うレジスタを全部保存しておく(呼び出し元の値を壊さない)
    xor bx, bx          ; bh = 表示するページ番号(0)
.loop:
    lodsb               ; al = [si] を読み込み、si を1つ進める
    test al, al         ; 0(文字列の終わり)なら
    jz .done            ;   ループを抜ける
    mov ah, 0x0e        ; BIOSの「1文字表示」機能
    int 0x10
    jmp .loop
.done:
    popa                ; 保存したレジスタを元に戻す
    ret
