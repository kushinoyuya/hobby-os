/*
 * keyboard.h - PS/2 キーボード
 */
#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);

/* 入力された文字を1つ取り出す。何もなければ -1 */
int keyboard_getchar(void);

#endif
