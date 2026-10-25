/* USB setup listener (1.2.47): protocol v1 (usb_proto.h) on the board's own
 * USB port. Call once from app_main, before Wi-Fi. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void usb_cmd_start(void);

#ifdef __cplusplus
}
#endif
