/*
 * Holds the panel's display-on command back until the first real frame is drawn, so power-on does
 * not flash white. See panel_gate.c.
 */
#pragma once

/**
 * @brief Switch the panel on, if the BSP's display-on was held back. Idempotent and cheap.
 *
 * Call once LVGL has flushed a frame worth showing. Until then the panel stays dark, however much
 * is written to its memory.
 */
void panel_gate_open(void);
