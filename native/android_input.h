#ifndef CF_ANDROID_INPUT_H
#define CF_ANDROID_INPUT_H
#include <jni.h>
#include "scene_snapshot.h"
bool cf_android_input_start(JavaVM *);
void cf_input_snapshot(const CfSceneSnapshot *);
void cf_input_enable(bool);
void cf_input_shutdown(void);
int cf_input_status(void);
#endif
