#ifndef __EVTUTILS_H__
#define __EVTUTILS_H__

#include "report.h"
#include "state.h"
#include "state_private.h"


//
// We use circular buffers to transfer events, logs/console, and payloads
// from the datapath (application's threads) to our own reporting
// (aka periodic) thread (periodic thread).
//
// ------------- ---------------- ----------------------
// category of  :    data type   :    circular buffer
//    data      :                :
// ------------- ---------------- ----------------------
//       events :      evt_type  : ctl->events
// logs/console :   log_event_t  : ctl->log.ringbuf
//     payloads : payload_info_t : ctl->payload.ringbuf
//
//
// When the datapath allocates events but can not add them to a circular
// buffer, we need to be able to deallocate them so we don't leak memory.
// When our reporting thread is done processing events, we want to do the
// exact same deallocation.
//
// evtutils was created as a common place for this allocation/deallocation
// code - particularly for the deallocation code that should be common
// between the datapath side (for error cases), and the reporting side.
//
//
// At this time, we're just starting with events, but hope to migrate
// code here for logs/console and payloads over time.

protocol_info * evtProtoCreate(void);
bool evtProtoDelete(protocol_info *proto);

bool evtDelete(evt_type *event);



#endif // __EVTUTILS_H__
