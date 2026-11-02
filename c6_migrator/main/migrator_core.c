#include <string.h>
#include "c6_board.h"
#include "migrator_core.h"

#define CHUNK            MIGRATOR_SECTOR
#define REGION_ATTEMPTS  2

static uint8_t s_a[CHUNK];
static uint8_t s_b[CHUNK];

static uint32_t round_up(uint32_t v)
{
    return (v + MIGRATOR_SECTOR - 1) & ~(uint32_t)(MIGRATOR_SECTOR - 1);
}

static bool overlaps_targets(uint32_t offset, uint32_t len)
{
    return offset < C6_BUNDLE_TARGET_END && len > 0; // every target lies in [0, C6_BUNDLE_TARGET_END)
}

migrator_action_t migrator_decide(const migrator_situation_t *s, migrator_slot_t *relocate_to)
{
    if (s->ptable_is_ours) {
        return MIGRATOR_ALREADY_OURS;
    }
    if (!overlaps_targets(s->running.offset, s->used)) {
        return MIGRATOR_INSTALL;
    }
    for (size_t i = 0; i < s->slot_count; i++) {
        const migrator_slot_t *c = &s->slots[i];
        if (c->offset != s->running.offset && c->offset >= C6_BUNDLE_TARGET_END && c->size >= s->used) {
            *relocate_to = *c;
            return MIGRATOR_RELOCATE;
        }
    }
    return MIGRATOR_STUCK;
}

uint32_t migrator_blob_addr(const c6_bundle_t *bundle, uint32_t data_addr, c6_bundle_kind_t kind)
{
    uint32_t addr = data_addr;

    for (int i = 0; i < C6_BUNDLE_ENTRIES && bundle->entries[i].kind != (uint32_t)kind; i++) {
        addr += bundle->entries[i].length;
    }
    return addr;
}

static const c6_bundle_entry_t *entry(const c6_bundle_t *bundle, c6_bundle_kind_t kind)
{
    return &bundle->entries[kind - 1];
}

esp_err_t migrator_check_blobs(const migrator_flash_t *flash, const c6_bundle_t *bundle, uint32_t data_addr)
{
    for (int k = C6_BUNDLE_BOOTLOADER; k <= C6_BUNDLE_RCP; k++) {
        const c6_bundle_entry_t *e = entry(bundle, (c6_bundle_kind_t)k);
        const uint32_t src = migrator_blob_addr(bundle, data_addr, (c6_bundle_kind_t)k);
        c6_bundle_sha256_t sha;
        uint8_t digest[C6_BUNDLE_SHA256_LEN];

        esp_err_t err = c6_bundle_sha256_begin(&sha);
        for (uint32_t done = 0; err == ESP_OK && done < e->length; done += CHUNK) {
            const size_t n = e->length - done < CHUNK ? e->length - done : CHUNK;
            err = flash->read(src + done, s_a, n);
            if (err == ESP_OK) {
                err = c6_bundle_sha256_update(&sha, s_a, n);
            }
        }
        if (err != ESP_OK) {
            c6_bundle_sha256_abort(&sha);
            return err;
        }
        err = c6_bundle_sha256_finish(&sha, digest);
        if (err != ESP_OK) {
            return err;
        }
        if (memcmp(digest, e->sha256, sizeof(digest)) != 0) {
            return ESP_ERR_INVALID_CRC;
        }
    }
    return ESP_OK;
}

/* Byte compare of [a, a + len) and [b, b + len). ESP_ERR_INVALID_CRC on a difference. */
static esp_err_t compare(const migrator_flash_t *flash, uint32_t a, uint32_t b, uint32_t len)
{
    for (uint32_t done = 0; done < len; done += CHUNK) {
        const size_t n = len - done < CHUNK ? len - done : CHUNK;
        esp_err_t err = flash->read(a + done, s_a, n);
        if (err == ESP_OK) {
            err = flash->read(b + done, s_b, n);
        }
        if (err != ESP_OK) {
            return err;
        }
        if (memcmp(s_a, s_b, n) != 0) {
            return ESP_ERR_INVALID_CRC;
        }
    }
    return ESP_OK;
}

/* Erase the sectors of [dst, dst + len) and copy [src, src + len) there. */
static esp_err_t copy(const migrator_flash_t *flash, uint32_t src, uint32_t dst, uint32_t len)
{
    esp_err_t err = flash->erase(dst, round_up(len));

    for (uint32_t done = 0; err == ESP_OK && done < len; done += CHUNK) {
        const size_t n = len - done < CHUNK ? len - done : CHUNK;
        err = flash->read(src + done, s_a, n);
        if (err == ESP_OK) {
            err = flash->write(dst + done, s_a, n);
        }
    }
    return err;
}

bool migrator_ptable_is_ours(const migrator_flash_t *flash, const c6_bundle_t *bundle, uint32_t data_addr)
{
    return compare(flash, C6_FLASH_PTABLE_OFFSET, migrator_blob_addr(bundle, data_addr, C6_BUNDLE_PTABLE),
                   entry(bundle, C6_BUNDLE_PTABLE)->length) == ESP_OK;
}

static esp_err_t place(const migrator_flash_t *flash, const c6_bundle_t *bundle, uint32_t data_addr, c6_bundle_kind_t kind)
{
    const uint32_t src = migrator_blob_addr(bundle, data_addr, kind);
    const uint32_t dst = c6_bundle_target_offset(kind);
    const uint32_t len = entry(bundle, kind)->length;
    const bool is_app = kind == C6_BUNDLE_RCP || kind == C6_BUNDLE_UPDATER;

    if (compare(flash, dst, src, len) == ESP_OK) {
        return ESP_OK; // already in place (a repeat after a power cut)
    }
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < REGION_ATTEMPTS; attempt++) {
        err = copy(flash, src, dst, len);
        if (err == ESP_OK) {
            err = compare(flash, dst, src, len);
        }
        if (err == ESP_OK && is_app) {
            err = flash->verify_image(dst, c6_bundle_target_room(kind));
        }
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
    return err;
}

esp_err_t migrator_install(const migrator_flash_t *flash, const c6_bundle_t *bundle, uint32_t data_addr)
{
    // RCP and updater first: until the bootloader and the partition table change, the stock ones still boot this migrator,
    // so a power cut there only repeats the run. The bootloader goes before the partition table: with our bootloader and
    // the stock table the C6 still boots the migrator.
    static const c6_bundle_kind_t order[] = {C6_BUNDLE_RCP, C6_BUNDLE_UPDATER, C6_BUNDLE_BOOTLOADER, C6_BUNDLE_PTABLE};

    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        esp_err_t err = place(flash, bundle, data_addr, order[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return flash->erase(C6_FLASH_OTADATA_OFFSET, C6_FLASH_OTADATA_SIZE);
}

esp_err_t migrator_relocate(const migrator_flash_t *flash, uint32_t from, migrator_slot_t to, uint32_t len)
{
    if (len == 0 || round_up(len) > to.size) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = copy(flash, from, to.offset, len);
    return err == ESP_OK ? compare(flash, to.offset, from, len) : err;
}
