#ifndef PAIR_UI_H
#define PAIR_UI_H

#ifdef __cplusplus
extern "C" {
#endif

// "PAIR YOUR ROD" screens, used instead of watch_ui_create() while the
// watch has no rod (lcd_bsp.c decides).
void pair_ui_create(void);

#ifdef __cplusplus
}
#endif

#endif
