#include "settings_store.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "cJSON.h"

static const char *TAG = "settings_store";

#define LFS_BASE_PATH     "/littlefs"
#define LFS_LABEL         "config"
#define SETTINGS_PATH     LFS_BASE_PATH "/settings.json"
#define SETTINGS_TMP_PATH LFS_BASE_PATH "/settings.json.tmp"

static settings_t s_settings = {
    .name = "Home Energy Manager",
};

static esp_err_t mount_littlefs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = LFS_BASE_PATH,
        .partition_label        = LFS_LABEL,
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: 0x%x", err);
        return err;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(LFS_LABEL, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at %s (used %u / %u bytes)", LFS_BASE_PATH, used, total);
    return ESP_OK;
}

static void load_from_disk(void)
{
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No settings file; using defaults");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4096) { fclose(f); return; }

    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(name))
        strlcpy(s_settings.name, name->valuestring, sizeof(s_settings.name));
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded settings: name=%s", s_settings.name);
}

static esp_err_t persist(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "name", s_settings.name);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;

    esp_err_t result = ESP_OK;
    FILE *f = fopen(SETTINGS_TMP_PATH, "w");
    if (!f) { result = ESP_FAIL; goto done; }
    if (fputs(text, f) == EOF) { fclose(f); result = ESP_FAIL; goto done; }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(SETTINGS_TMP_PATH, SETTINGS_PATH) != 0)
        result = ESP_FAIL;
done:
    free(text);
    return result;
}

esp_err_t settings_store_init(void)
{
    esp_err_t err = mount_littlefs();
    if (err != ESP_OK) return err;
    load_from_disk();
    return ESP_OK;
}

const settings_t *settings_store_get(void)
{
    return &s_settings;
}

esp_err_t settings_store_update(const char *name, settings_t *out)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    settings_t backup = s_settings;
    strlcpy(s_settings.name, name, sizeof(s_settings.name));
    esp_err_t err = persist();
    if (err != ESP_OK) { s_settings = backup; return err; }
    if (out) *out = s_settings;
    return ESP_OK;
}
