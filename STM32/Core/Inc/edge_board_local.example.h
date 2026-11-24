/* Copy to edge_board_local.h and fill from the verified board schematic.
 * All-zero pins disable their peripheral. OLED needs a confirmed SSD1306
 * 128x64 panel. No example pin here is intended as a wiring instruction.
 */
#define EDGE_BOARD_CONFIG { \
    .dht = {0}, .trig = {0}, .echo = {0}, \
    .scl = {0}, .sda = {0}, .oled_address = 0, \
    .tx = {0}, .rx = {0}, \
}
