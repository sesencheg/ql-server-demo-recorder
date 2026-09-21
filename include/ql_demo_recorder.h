#ifndef QL_DEMO_RECORDER_H
#define QL_DEMO_RECORDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void qldr_init(void);
void qldr_shutdown(int restore_client_state);

#ifdef __cplusplus
}
#endif

#endif
