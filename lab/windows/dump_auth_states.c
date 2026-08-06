/*
 * Read-only authorization-state probe for the official Windows client.
 *
 * This program calls the same public client-library method used by UADPerfMon
 * to retrieve the 20-byte per-product authorization records. It does not
 * alter authorizations, submit plug-in resources, or write card registers.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUTH_PRODUCT_COUNT 0x300u
#define MAX_DEVICE_COUNT 32u

#pragma pack(push, 1)
struct auth_record {
    uint32_t product_id;
    uint32_t state;
    uint32_t auxiliary_08;
    uint32_t demo_days;
    uint32_t auxiliary_10;
};
#pragma pack(pop)

typedef void *(__cdecl *open_uad2_driver_fn)(const char *application_name,
                                              int client_type);
typedef int64_t(__fastcall *release_driver_fn)(void *driver);
typedef int32_t(__fastcall *get_auth_records_fn)(
    void *driver,
    uint32_t device_index,
    struct auth_record *records,
    uint32_t first_product,
    uint32_t product_count,
    uint32_t *returned_count);

static void print_record(uint32_t device_index,
                         uint32_t index,
                         const struct auth_record *record)
{
    printf("record device=%" PRIu32 " index=%" PRIu32
           " product=0x%08" PRIx32 " state=%" PRIu32
           " aux08=0x%08" PRIx32 " days=%" PRIu32
           " aux10=0x%08" PRIx32 "\n",
           device_index,
           index,
           record->product_id,
           record->state,
           record->auxiliary_08,
           record->demo_days,
           record->auxiliary_10);
}

int main(void)
{
    HMODULE module;
    open_uad2_driver_fn open_driver;
    release_driver_fn release_driver;
    get_auth_records_fn get_auth_records;
    struct auth_record *records;
    void *driver;
    void **vtable;
    uint32_t device_index;
    uint32_t successful_devices = 0;

    if (sizeof(struct auth_record) != 20u) {
        fprintf(stderr, "unexpected auth_record size: %zu\n",
                sizeof(struct auth_record));
        return 2;
    }

    module = LoadLibraryA("UAD2DriverClient.dll");
    if (module == NULL) {
        fprintf(stderr, "LoadLibraryA failed: %lu\n", GetLastError());
        return 3;
    }

    open_driver = (open_uad2_driver_fn)(uintptr_t)GetProcAddress(
        module, "OpenUAD2Driver");
    if (open_driver == NULL) {
        fprintf(stderr, "GetProcAddress failed: %lu\n", GetLastError());
        FreeLibrary(module);
        return 4;
    }

    driver = open_driver("UAD authorization state probe", 15);
    if (driver == NULL) {
        fprintf(stderr, "OpenUAD2Driver returned NULL\n");
        FreeLibrary(module);
        return 5;
    }

    vtable = *(void ***)driver;
    release_driver = (release_driver_fn)(uintptr_t)vtable[2];
    get_auth_records = (get_auth_records_fn)(uintptr_t)vtable[10];
    records = (struct auth_record *)calloc(AUTH_PRODUCT_COUNT,
                                           sizeof(*records));
    if (records == NULL) {
        fprintf(stderr, "allocation failed\n");
        release_driver(driver);
        FreeLibrary(module);
        return 6;
    }

    printf("schema product_id,state,auxiliary_08,demo_days,auxiliary_10\n");
    for (device_index = 0; device_index < MAX_DEVICE_COUNT; ++device_index) {
        uint32_t returned_count = 0;
        uint32_t state_counts[16] = {0};
        uint32_t unknown_state_count = 0;
        uint32_t index;
        int32_t status;

        memset(records, 0, AUTH_PRODUCT_COUNT * sizeof(*records));
        status = get_auth_records(driver,
                                  device_index,
                                  records,
                                  0,
                                  AUTH_PRODUCT_COUNT,
                                  &returned_count);
        if (status < 0) {
            printf("device=%" PRIu32 " status=%" PRId32
                   " returned=%" PRIu32 "\n",
                   device_index, status, returned_count);
            if (successful_devices != 0u)
                break;
            continue;
        }

        ++successful_devices;
        if (returned_count > AUTH_PRODUCT_COUNT)
            returned_count = AUTH_PRODUCT_COUNT;
        for (index = 0; index < returned_count; ++index) {
            uint32_t state = records[index].state;

            if (state < 16u)
                ++state_counts[state];
            else
                ++unknown_state_count;
        }

        printf("device=%" PRIu32 " status=%" PRId32
               " returned=%" PRIu32,
               device_index, status, returned_count);
        for (index = 0; index < 16u; ++index) {
            if (state_counts[index] != 0u)
                printf(" state_%" PRIu32 "=%" PRIu32,
                       index, state_counts[index]);
        }
        if (unknown_state_count != 0u)
            printf(" state_other=%" PRIu32, unknown_state_count);
        putchar('\n');

        for (index = 0; index < returned_count; ++index) {
            if (records[index].state != 4u ||
                records[index].demo_days != 0u ||
                records[index].auxiliary_08 != 0u ||
                records[index].auxiliary_10 != 0u)
                print_record(device_index, index, &records[index]);
        }
    }

    free(records);
    release_driver(driver);
    FreeLibrary(module);
    return successful_devices == 0u ? 7 : 0;
}
