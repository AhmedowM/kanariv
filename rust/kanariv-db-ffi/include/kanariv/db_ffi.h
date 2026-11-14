#ifndef KANARIV_DB_FFI_H
#define KANARIV_DB_FFI_H

/*
 * Read-only C ABI probe over the experimental Rust database crate.
 *
 * This header is used only by the FFI parity harness. The daemon does not
 * link the Rust library. Status codes: 0 = ok, 1 = invalid argument,
 * 2 = not found, 3 = internal. Strings returned through out-parameters are
 * owned by the caller and must be released with kanariv_db_ffi_string_free().
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KanarivDbHandle KanarivDbHandle;

KanarivDbHandle* kanariv_db_ffi_open(const char* path, char** error);
void kanariv_db_ffi_close(KanarivDbHandle* handle);
int32_t kanariv_db_ffi_search_json(KanarivDbHandle* handle, const char* query, int32_t limit,
                                   char** json, char** error);
int32_t kanariv_db_ffi_get_track_json(KanarivDbHandle* handle, int64_t id, char** json,
                                      char** error);
void kanariv_db_ffi_string_free(char* value);

#ifdef __cplusplus
}
#endif

#endif /* KANARIV_DB_FFI_H */
