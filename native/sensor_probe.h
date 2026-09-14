#ifndef CF_SENSOR_PROBE_H
#define CF_SENSOR_PROBE_H
#include "readonly_module.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Filter for Unity's NDK sensor queue. Installation redirects only the
 * writable ASensorEventQueue_getEvents GOT slot. Non-gyro events remain
 * untouched; bounded X/Y corrections can be added to type-4 gyro events.
 */
bool cf_sensor_probe_install(const CfModule *, char *, size_t);
void cf_sensor_probe_set_adjustment(bool, float, float);
void cf_sensor_probe_shutdown(void);

#endif
