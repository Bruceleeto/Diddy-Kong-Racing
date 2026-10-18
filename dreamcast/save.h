#ifndef DREAMCAST_SAVE_H
#define DREAMCAST_SAVE_H

#include <kos.h>

#ifdef __cplusplus
extern "C" {
#endif

int eeprom_flush_to_vmu(void);
void eeprom_update(void);

#ifdef __cplusplus
}
#endif

#endif // DREAMCAST_SAVE_H
