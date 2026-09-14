#ifndef CF_IL2CPP_API_H
#define CF_IL2CPP_API_H
#include "readonly_exports.h"
typedef struct MetadataApi {
    void *(*domain_get)(void);
    const void **(*domain_get_assemblies)(const void *, size_t *);
    const void *(*assembly_get_image)(const void *);
    const char *(*image_get_name)(const void *);
    void *(*class_from_name)(const void *, const char *, const char *);
    void *(*class_get_field_from_name)(void *, const char *);
    uint32_t (*class_instance_size)(void *);
    size_t (*field_get_offset)(void *);
    int (*field_get_flags)(void *);
    void *(*field_get_type)(void *);
    int (*type_get_type)(void *);
    void (*field_static_get_value)(void *, void *);
    void *(*thread_current)(void);
    void *(*thread_attach)(void *);
    void (*thread_detach)(void *);
} MetadataApi;
bool cf_metadata_api_open(const CfExports *, MetadataApi *, char *, size_t);
#endif
