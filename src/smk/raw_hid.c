#include "raw_hid.h"

#ifdef VIA_ENABLE

#    include "usb.h"
#    include "usbhw.h"
#    include "via.h"
#    include <string.h>

static __xdata uint8_t request[RAW_HID_REPORT_SIZE];
/* Shared between the main loop and USB completion/RX interrupts. */
static volatile bool request_pending;
static volatile bool response_pending;
#    ifdef ISP_ENABLE
/* Set when VIA_ID_BOOTLOADER_JUMP is accepted; main loop waits for EP2 IN ACK. */
static volatile bool isp_after_response;
#    endif

void raw_hid_init(void)
{
    request_pending  = false;
    response_pending = false;
#    ifdef ISP_ENABLE
    isp_after_response = false;
#    endif
    SET_EP2_OUT_RDY;
}

#    ifdef ISP_ENABLE
void raw_hid_request_isp(void)
{
    isp_after_response = true;
}
#    endif

void raw_hid_receive_irq(void)
{
    if (request_pending || response_pending) {
        return;
    }
    if (OEP2CNT != RAW_HID_REPORT_SIZE) {
        SET_EP2_OUT_RDY;
        return;
    }
    /* Keep the packet in EP2_OUT_BUF until raw_hid_task() runs in the main loop:
     * OUT stays NAKed (no SET_EP2_OUT_RDY) until processed, so copying in main
     * keeps _INT_USB short and prevents EP2 OUT bursts from delaying Timer2. */
    request_pending = true;
}

void raw_hid_task(void)
{
#    ifdef ISP_ENABLE
    /* Wait until the host ACKs our 0x0B reply on EP2 IN before detaching. */
    if (isp_after_response && !response_pending) {
        isp_after_response = false;
        usb_request_isp();
        return;
    }
#    endif
    if (!request_pending || !usb_is_configured() || !usb_hw_ep2_in_free()) {
        return;
    }

    for (uint8_t i = 0; i < RAW_HID_REPORT_SIZE; i++) {
        request[i] = EP2_OUT_BUF[i];
    }

    if (!via_process_packet(request)) {
        request_pending = false;
        SET_EP2_OUT_RDY;
        return;
    }
    /* Publish ownership BEFORE arming EP2. A completion can run as soon as
     * the hardware sees IN_RDY. Never overwrite its state after submission.
     * OUT stays unarmed until that completion, protecting request[] while
     * via_process_packet and the copy to the endpoint are in progress. */
    request_pending  = false;
    response_pending = true;
    usb_hw_ep2_in_send(request, RAW_HID_REPORT_SIZE);
}

void raw_hid_in_complete(void)
{
    /* Ignore unrelated/stale completion: it must not reopen OUT while a
     * request is still owned by the foreground processor. */
    if (response_pending) {
        response_pending = false;
        SET_EP2_OUT_RDY;
    }
}

#endif
