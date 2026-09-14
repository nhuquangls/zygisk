#ifndef CF_READONLY_RUNTIME_H
#define CF_READONLY_RUNTIME_H
enum CfRuntimeStatus {
    CF_STOPPED = 0,
    CF_WAITING_LIBRARY = 1,
    CF_LIBRARY_TIMEOUT = 2,
    CF_READ_UNAVAILABLE = 3,
    CF_NEEDS_VERIFIED_BINDINGS = 4,
    CF_METADATA_UNAVAILABLE = 5,
    CF_WAITING_SCENE = 6,
    CF_INPUT_READY = 7,
    CF_INPUT_UNAVAILABLE = 8,
    CF_INPUT_ERROR = 9,
    CF_VIEWPORT_UNSUPPORTED = 10
};
int cf_payload_start(void);
int cf_payload_status(void);
#endif
