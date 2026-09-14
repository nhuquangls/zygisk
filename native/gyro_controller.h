#ifndef CF_GYRO_CONTROLLER_H
#define CF_GYRO_CONTROLLER_H
#include "aim_policy.h"

#define CF_GYRO_TRIGGER_LOST_GRACE_NS 500000000ULL
#define CF_GYRO_TARGET_GRACE_NS 200000000ULL
#define CF_GYRO_LOCK_TIMEOUT_NS 2000000000ULL
#define CF_GYRO_DEADBAND_PX 3.5f
#define CF_GYRO_RATE_CAP 0.15f
#define CF_GYRO_KP 5.5f

enum CfGyroMode {
    CF_GYRO_IDLE = 0,
    CF_GYRO_TRACKING = 2,
    CF_GYRO_LATCHED = 3
};

typedef struct CfGyroController {
    CfTargetPredictor predictor;
    uint64_t locked_target;
    uint64_t last_seen_ns;
    uint64_t lock_started_ns;
    uint64_t trigger_lost_ns;
    float filtered_x;
    float filtered_y;
    bool settled;
    bool trigger_armed;
} CfGyroController;

typedef struct CfGyroCommand {
    uint64_t target_id;
    float error_x;
    float error_y;
    float sensor_x;
    float sensor_y;
    uint32_t mode;
    bool active;
    bool settled;
} CfGyroCommand;

void cf_gyro_controller_reset(CfGyroController *);
bool cf_gyro_controller_step(CfGyroController *, const CfSceneSnapshot *,
                             const CfViewport *, uint64_t, CfGyroCommand *);

#endif
