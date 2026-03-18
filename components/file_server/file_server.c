#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs.h"
#include "esp_http_server.h"
#include "file_server.h"

static const char *TAG = "file_server";
static char s_base_path[ESP_VFS_PATH_MAX + 1];
static httpd_handle_t s_server = NULL;

static void log_heap_state(const char *label)
{
    ESP_LOGI(TAG, "%s | free_heap=%lu largest_block=%lu",
             label,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static const char *HTML_INDEX_HEADER =
    "<!DOCTYPE html><html><head><title>KidBox Manager</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>"
    ":root { color-scheme: dark; }"
    "body { font-family: sans-serif; background: #1b1d22; color: #f4f7fb; margin: 0; padding: 20px; }"
    ".shell { max-width: 960px; margin: 0 auto; }"
    ".toolbar, .pathbar, .status { background: #262a31; border: 1px solid #3a404a; border-radius: 12px; padding: 16px; margin-bottom: 16px; }"
    ".toolbar { display: grid; gap: 12px; }"
    ".toolbar-row { display: flex; gap: 10px; flex-wrap: wrap; align-items: center; }"
    "table { width: 100%; border-collapse: collapse; overflow: hidden; background: #262a31; border: 1px solid #3a404a; border-radius: 12px; }"
    "th, td { border-bottom: 1px solid #343943; padding: 10px 12px; text-align: left; }"
    "th { color: #9fb3c8; font-weight: 600; }"
    "tr:last-child td { border-bottom: none; }"
    "a { color: #8fd3ff; text-decoration: none; }"
    "a:hover { text-decoration: underline; }"
    ".btn { background: #8fd3ff; color: #0f1419; padding: 8px 12px; border: none; border-radius: 8px; cursor: pointer; font-weight: 700; }"
    ".btn.danger { background: #ff7b72; }"
    ".btn.secondary { background: #4a5563; color: #f4f7fb; }"
    "input[type='file'], input[type='text'] { background: #17191e; color: #f4f7fb; border: 1px solid #424854; border-radius: 8px; padding: 8px 10px; }"
    ".muted { color: #9fb3c8; }"
    "@media (max-width: 640px) { body { padding: 12px; } th:nth-child(2), td:nth-child(2) { display: none; } }"
    "</style>"
    "<script>";

static const char *HTML_INDEX_SCRIPT_FOOTER =
    "function setStatus(text) { document.getElementById('status').innerText = text; }"
    "function refreshPage() { window.location = '/?path=' + encodeURIComponent(currentPath); }"
    "async function uploadOne(file, index, total) {"
    "  setStatus('Uploading ' + (index+1) + ' / ' + total + ': ' + file.name + '...');"
    "  const resp = await fetch('/upload?path=' + encodeURIComponent(currentPath) + '&filename=' + encodeURIComponent(file.name), { method: 'POST', body: file });"
    "  const text = await resp.text();"
    "  if (!resp.ok) throw new Error(text || 'Upload failed: ' + file.name);"
    "}"
    "async function upload(files) {"
    "  if (!files || files.length === 0) return;"
    "  const total = files.length;"
    "  try {"
    "    for (let i = 0; i < total; i++) { await uploadOne(files[i], i, total); }"
    "    setStatus('Done! ' + total + ' file' + (total > 1 ? 's' : '') + ' uploaded.');"
    "    refreshPage();"
    "  } catch(err) { setStatus(err.message); }"
    "  document.getElementById('file_input').value = '';"
    "}"
    "function createFolder() {"
    "  const name = document.getElementById('folder_name').value.trim();"
    "  if (!name) { setStatus('Enter a folder name first'); return; }"
    "  setStatus('Creating folder ' + name + '...');"
    "  fetch('/mkdir?path=' + encodeURIComponent(currentPath) + '&name=' + encodeURIComponent(name), { method: 'POST' })"
    "    .then(async resp => { const text = await resp.text(); if (!resp.ok) throw new Error(text || 'Create folder failed'); document.getElementById('folder_name').value = ''; setStatus(text || 'Folder created'); refreshPage(); })"
    "    .catch(err => setStatus(err.message));"
    "}"
    "function deleteEntry(path) {"
    "  if (!confirm('Delete ' + path + '?')) return;"
    "  setStatus('Deleting ' + path + '...');"
    "  fetch('/delete?path=' + encodeURIComponent(path), { method: 'DELETE' })"
    "    .then(async resp => { const text = await resp.text(); if (!resp.ok) throw new Error(text || 'Delete failed'); setStatus(text || 'Deleted'); refreshPage(); })"
    "    .catch(err => setStatus(err.message));"
    "}"
    "</script></head><body><div class='shell'>";

static const char *HTML_INDEX_FOOTER = "</div></body></html>";

static bool build_path(char *dst, size_t dst_size, const char *base, const char *suffix, bool with_slash)
{
    size_t base_len = strlen(base);
    size_t suffix_len = strlen(suffix);
    size_t needed = base_len + (with_slash ? 1 : 0) + suffix_len + 1;

    if (needed > dst_size) {
        return false;
    }

    memcpy(dst, base, base_len);
    size_t pos = base_len;
    if (with_slash) {
        dst[pos++] = '/';
    }
    memcpy(dst + pos, suffix, suffix_len + 1);
    return true;
}

static esp_err_t send_chunkf(httpd_req_t *req, const char *fmt, ...)
{
    va_list args_copy;
    va_list args;

    va_start(args, fmt);
    va_copy(args_copy, args);
    int written = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (written < 0) {
        va_end(args);
        ESP_LOGE(TAG, "Failed to format HTTP chunk");
        return ESP_FAIL;
    }

    char *buffer = malloc((size_t)written + 1);
    if (!buffer) {
        va_end(args);
        ESP_LOGE(TAG, "Out of memory while formatting HTTP chunk");
        return ESP_ERR_NO_MEM;
    }

    vsnprintf(buffer, (size_t)written + 1, fmt, args);
    va_end(args);

    esp_err_t err = httpd_resp_sendstr_chunk(req, buffer);
    free(buffer);
    return err;
}

static bool url_decode(char *dst, size_t dst_size, const char *src)
{
    size_t pos = 0;

    while (*src != '\0') {
        if (pos + 1 >= dst_size) {
            return false;
        }

        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[pos++] = (char)strtol(hex, NULL, 16);
            src += 3;
            continue;
        }

        if (*src == '+') {
            dst[pos++] = ' ';
            src++;
            continue;
        }

        dst[pos++] = *src++;
    }

    dst[pos] = '\0';
    return true;
}

static bool uri_encode(const char *src, char *dst, size_t dst_size, bool preserve_slash)
{
    static const char *hex = "0123456789ABCDEF";
    size_t pos = 0;

    while (*src != '\0') {
        unsigned char ch = (unsigned char)*src++;
        bool is_unreserved = isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';

        if (is_unreserved || (preserve_slash && ch == '/')) {
            if (pos + 1 >= dst_size) {
                return false;
            }
            dst[pos++] = (char)ch;
            continue;
        }

        if (pos + 3 >= dst_size) {
            return false;
        }

        dst[pos++] = '%';
        dst[pos++] = hex[ch >> 4];
        dst[pos++] = hex[ch & 0x0F];
    }

    dst[pos] = '\0';
    return true;
}

static bool html_escape(const char *src, char *dst, size_t dst_size)
{
    size_t pos = 0;

    while (*src != '\0') {
        const char *replacement = NULL;

        switch (*src) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '\"': replacement = "&quot;"; break;
            case '\'': replacement = "&#39;"; break;
            default: break;
        }

        if (replacement) {
            size_t repl_len = strlen(replacement);
            if (pos + repl_len >= dst_size) {
                return false;
            }
            memcpy(dst + pos, replacement, repl_len);
            pos += repl_len;
        } else {
            if (pos + 1 >= dst_size) {
                return false;
            }
            dst[pos++] = *src;
        }

        src++;
    }

    dst[pos] = '\0';
    return true;
}

static bool normalize_relative_path(const char *raw_path, char *normalized, size_t normalized_size)
{
    size_t pos = 0;
    const char *cursor = raw_path;

    if (!raw_path || !*raw_path || strcmp(raw_path, "/") == 0) {
        normalized[0] = '\0';
        return true;
    }

    while (*cursor == '/' || *cursor == '\\') {
        cursor++;
    }

    while (*cursor != '\0') {
        const char *segment = cursor;
        size_t segment_len = 0;

        while (*cursor != '\0' && *cursor != '/' && *cursor != '\\') {
            if ((unsigned char)*cursor < 0x20) {
                return false;
            }
            cursor++;
            segment_len++;
        }

        while (*cursor == '/' || *cursor == '\\') {
            cursor++;
        }

        if (segment_len == 0) {
            continue;
        }

        if ((segment_len == 1 && segment[0] == '.') ||
            (segment_len == 2 && segment[0] == '.' && segment[1] == '.')) {
            return false;
        }

        if (pos != 0) {
            if (pos + 1 >= normalized_size) {
                return false;
            }
            normalized[pos++] = '/';
        }

        if (pos + segment_len >= normalized_size) {
            return false;
        }

        memcpy(normalized + pos, segment, segment_len);
        pos += segment_len;
    }

    normalized[pos] = '\0';
    return true;
}

static bool build_fs_path_from_relative(char *dst, size_t dst_size, const char *relative_path)
{
    if (!relative_path || relative_path[0] == '\0') {
        return strlcpy(dst, s_base_path, dst_size) < dst_size;
    }

    return build_path(dst, dst_size, s_base_path, relative_path, true);
}

static bool relative_to_display_path(const char *relative_path, char *display_path, size_t display_size)
{
    if (!relative_path || relative_path[0] == '\0') {
        return strlcpy(display_path, "/", display_size) < display_size;
    }

    return snprintf(display_path, display_size, "/%s", relative_path) < (int)display_size;
}

static bool join_relative_path(const char *dir_relative, const char *name, char *out, size_t out_size)
{
    if (!name || !*name) {
        return false;
    }

    if (strchr(name, '/') || strchr(name, '\\')) {
        return false;
    }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }

    if (!dir_relative || dir_relative[0] == '\0') {
        return strlcpy(out, name, out_size) < out_size;
    }

    return snprintf(out, out_size, "%s/%s", dir_relative, name) < (int)out_size;
}

static bool parent_relative_path(const char *relative_path, char *parent, size_t parent_size)
{
    char temp[256];
    char *last_slash;

    if (!relative_path || relative_path[0] == '\0') {
        parent[0] = '\0';
        return true;
    }

    if (strlcpy(temp, relative_path, sizeof(temp)) >= sizeof(temp)) {
        return false;
    }

    last_slash = strrchr(temp, '/');
    if (!last_slash) {
        parent[0] = '\0';
        return true;
    }

    *last_slash = '\0';
    return strlcpy(parent, temp, parent_size) < parent_size;
}

static bool get_query_value_decoded(httpd_req_t *req, const char *key, char *dst, size_t dst_size)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    esp_err_t err;
    char *query;
    char value[256];

    if (query_len == 0) {
        return false;
    }

    query = malloc(query_len + 1);
    if (!query) {
        return false;
    }

    err = httpd_req_get_url_query_str(req, query, query_len + 1);
    if (err != ESP_OK) {
        free(query);
        return false;
    }

    err = httpd_query_key_value(query, key, value, sizeof(value));
    free(query);
    if (err != ESP_OK) {
        return false;
    }

    return url_decode(dst, dst_size, value);
}

static bool extract_uri_path(httpd_req_t *req, char *dst, size_t dst_size)
{
    const char *query = strchr(req->uri, '?');
    size_t path_len = query ? (size_t)(query - req->uri) : strlen(req->uri);

    if (path_len + 1 > dst_size) {
        return false;
    }

    memcpy(dst, req->uri, path_len);
    dst[path_len] = '\0';
    return true;
}

static const char *content_type_for_path(const char *path)
{
    const char *ext = strrchr(path, '.');

    if (!ext) {
        return "application/octet-stream";
    }

    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) return "text/html";
    if (strcasecmp(ext, ".css") == 0) return "text/css";
    if (strcasecmp(ext, ".js") == 0) return "application/javascript";
    if (strcasecmp(ext, ".png") == 0) return "image/png";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".gif") == 0) return "image/gif";
    if (strcasecmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, ".txt") == 0) return "text/plain";
    if (strcasecmp(ext, ".json") == 0) return "application/json";
    if (strcasecmp(ext, ".wav") == 0) return "audio/wav";
    if (strcasecmp(ext, ".mp3") == 0) return "audio/mpeg";
    return "application/octet-stream";
}

static esp_err_t delete_path_recursive(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return ESP_FAIL;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;

        if (!dir) {
            return ESP_FAIL;
        }

        while ((entry = readdir(dir)) != NULL) {
            char child_path[512];

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            if (!build_path(child_path, sizeof(child_path), path, entry->d_name, true)) {
                closedir(dir);
                return ESP_FAIL;
            }

            if (delete_path_recursive(child_path) != ESP_OK) {
                closedir(dir);
                return ESP_FAIL;
            }
        }

        closedir(dir);
        return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
    }

    return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t download_get_handler(httpd_req_t *req)
{
    char raw_uri_path[256];
    char decoded_uri_path[256];
    char relative_path[256];
    char filepath[512];
    struct stat file_stat;
    FILE *fd;
    char *chunk;

    if (!extract_uri_path(req, raw_uri_path, sizeof(raw_uri_path)) ||
        !url_decode(decoded_uri_path, sizeof(decoded_uri_path), raw_uri_path) ||
        !normalize_relative_path(decoded_uri_path, relative_path, sizeof(relative_path)) ||
        !build_fs_path_from_relative(filepath, sizeof(filepath), relative_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    if (relative_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    if (stat(filepath, &file_stat) != 0 || S_ISDIR(file_stat.st_mode)) {
        ESP_LOGE(TAG, "File not found : %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    fd = fopen(filepath, "rb");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to read existing file : %s (%s)", filepath, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sending file : %s (%ld bytes)", filepath, (long)file_stat.st_size);
    httpd_resp_set_type(req, content_type_for_path(filepath));

    chunk = malloc(4096);
    if (!chunk) {
        fclose(fd);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    while (true) {
        size_t chunk_size = fread(chunk, 1, 4096, fd);
        if (chunk_size == 0) {
            break;
        }

        if (httpd_resp_send_chunk(req, chunk, chunk_size) != ESP_OK) {
            fclose(fd);
            free(chunk);
            ESP_LOGE(TAG, "File sending failed");
            return ESP_FAIL;
        }
    }

    fclose(fd);
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t index_html_get_handler(httpd_req_t *req)
{
    char requested_path[256] = "";
    char relative_path[256];
    char directory_path[512];
    char display_path[256];
    char encoded_display_path[768];
    char escaped_display_path[512];
    struct stat directory_stat;
    DIR *dir;
    struct dirent *entry;

    if (get_query_value_decoded(req, "path", requested_path, sizeof(requested_path))) {
        if (!normalize_relative_path(requested_path, relative_path, sizeof(relative_path))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid directory path");
            return ESP_FAIL;
        }
    } else {
        relative_path[0] = '\0';
    }

    if (!build_fs_path_from_relative(directory_path, sizeof(directory_path), relative_path) ||
        !relative_to_display_path(relative_path, display_path, sizeof(display_path)) ||
        !uri_encode(display_path, encoded_display_path, sizeof(encoded_display_path), false) ||
        !html_escape(display_path, escaped_display_path, sizeof(escaped_display_path))) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Path too long");
        return ESP_FAIL;
    }

    if (stat(directory_path, &directory_stat) != 0 || !S_ISDIR(directory_stat.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory does not exist");
        return ESP_FAIL;
    }

    dir = opendir(directory_path);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open directory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HTML_INDEX_HEADER);
    send_chunkf(req, "const currentPath = decodeURIComponent('%s');", encoded_display_path);
    httpd_resp_sendstr_chunk(req, HTML_INDEX_SCRIPT_FOOTER);
    httpd_resp_sendstr_chunk(req, "<h2>KidBox SD Card</h2>");
    send_chunkf(req, "<div class='pathbar'><strong>Current path:</strong> <span class='muted'>%s</span></div>", escaped_display_path);
    httpd_resp_sendstr_chunk(req, "<div class='toolbar'>");
    httpd_resp_sendstr_chunk(req, "<div class='toolbar-row'><input type='file' id='file_input' multiple onchange='upload(this.files)'><button class='btn secondary' onclick='document.getElementById(\"file_input\").click()'>Choose Files</button></div>");
    httpd_resp_sendstr_chunk(req, "<div class='toolbar-row'><input type='text' id='folder_name' placeholder='New folder name'><button class='btn' onclick='createFolder()'>Create Folder</button></div>");
    httpd_resp_sendstr_chunk(req, "</div>");
    httpd_resp_sendstr_chunk(req, "<div class='status'><span id='status'>Ready.</span></div>");
    httpd_resp_sendstr_chunk(req, "<table><tr><th>Name</th><th>Size</th><th>Action</th></tr>");

    if (relative_path[0] != '\0') {
        char parent_relative[256];
        char parent_display[256];
        char parent_encoded[768];

        if (parent_relative_path(relative_path, parent_relative, sizeof(parent_relative)) &&
            relative_to_display_path(parent_relative, parent_display, sizeof(parent_display)) &&
            uri_encode(parent_display, parent_encoded, sizeof(parent_encoded), false)) {
            send_chunkf(req, "<tr><td><a href='/?path=%s'>..</a></td><td class='muted'>dir</td><td></td></tr>", parent_encoded);
        }
    }

    while ((entry = readdir(dir)) != NULL) {
        char child_relative[256];
        char child_display[256];
        char child_fs_path[512];
        char escaped_name[384];
        char encoded_query_path[768];
        char encoded_link_path[768];
        char entry_size[32];
        struct stat entry_stat;
        bool is_dir;

        if (!join_relative_path(relative_path, entry->d_name, child_relative, sizeof(child_relative)) ||
            !relative_to_display_path(child_relative, child_display, sizeof(child_display)) ||
            !build_fs_path_from_relative(child_fs_path, sizeof(child_fs_path), child_relative) ||
            !html_escape(entry->d_name, escaped_name, sizeof(escaped_name))) {
            continue;
        }

        if (stat(child_fs_path, &entry_stat) != 0) {
            continue;
        }

        is_dir = S_ISDIR(entry_stat.st_mode);
        if (!uri_encode(child_display, encoded_query_path, sizeof(encoded_query_path), false) ||
            !uri_encode(child_display, encoded_link_path, sizeof(encoded_link_path), true)) {
            continue;
        }

        if (is_dir) {
            strcpy(entry_size, "dir");
            send_chunkf(req,
                        "<tr><td><a href='/?path=%s'>%s/</a></td><td class='muted'>%s</td><td><button class='btn danger' onclick=\"deleteEntry(decodeURIComponent('%s'))\">Delete</button></td></tr>",
                        encoded_query_path,
                        escaped_name,
                        entry_size,
                        encoded_query_path);
        } else {
            snprintf(entry_size, sizeof(entry_size), "%lu", (unsigned long)entry_stat.st_size);
            send_chunkf(req,
                        "<tr><td><a href='%s'>%s</a></td><td>%s</td><td><button class='btn danger' onclick=\"deleteEntry(decodeURIComponent('%s'))\">Delete</button></td></tr>",
                        encoded_link_path,
                        escaped_name,
                        entry_size,
                        encoded_query_path);
        }
    }

    closedir(dir);
    httpd_resp_sendstr_chunk(req, "</table>");
    httpd_resp_sendstr_chunk(req, HTML_INDEX_FOOTER);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char requested_path[256] = "";
    char filename[128];
    char relative_path[256];
    char target_relative[384];
    char filepath[512];
    char *buf;
    FILE *fd;
    int remaining = req->content_len;

    if (!get_query_value_decoded(req, "filename", filename, sizeof(filename)) ||
        !filename[0] || strchr(filename, '/') || strchr(filename, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    if (get_query_value_decoded(req, "path", requested_path, sizeof(requested_path))) {
        if (!normalize_relative_path(requested_path, relative_path, sizeof(relative_path))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid target directory");
            return ESP_FAIL;
        }
    } else {
        relative_path[0] = '\0';
    }

    if (!join_relative_path(relative_path, filename, target_relative, sizeof(target_relative)) ||
        !build_fs_path_from_relative(filepath, sizeof(filepath), target_relative)) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Path too long");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Receiving file: %s", filepath);
    fd = fopen(filepath, "wb");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to create file : %s (%s)", filepath, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
        return ESP_FAIL;
    }

    buf = malloc(4096);
    if (!buf) {
        fclose(fd);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int received = httpd_req_recv(req, buf, MIN(remaining, 4096));

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }

        if (received <= 0) {
            ESP_LOGE(TAG, "Upload receive failed for %s", filepath);
            fclose(fd);
            free(buf);
            unlink(filepath);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "File receive failed");
            return ESP_FAIL;
        }

        if ((int)fwrite(buf, 1, received, fd) != received) {
            ESP_LOGE(TAG, "File write failed for %s (%s)", filepath, strerror(errno));
            fclose(fd);
            free(buf);
            unlink(filepath);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "File write failed");
            return ESP_FAIL;
        }

        remaining -= received;
    }

    free(buf);
    fclose(fd);
    httpd_resp_sendstr(req, "File uploaded successfully");
    return ESP_OK;
}

static esp_err_t mkdir_post_handler(httpd_req_t *req)
{
    char requested_path[256] = "";
    char folder_name[128];
    char relative_path[256];
    char target_relative[384];
    char directory_path[512];

    if (!get_query_value_decoded(req, "name", folder_name, sizeof(folder_name)) ||
        !folder_name[0] || strchr(folder_name, '/') || strchr(folder_name, '\\') ||
        strcmp(folder_name, ".") == 0 || strcmp(folder_name, "..") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid folder name");
        return ESP_FAIL;
    }

    if (get_query_value_decoded(req, "path", requested_path, sizeof(requested_path))) {
        if (!normalize_relative_path(requested_path, relative_path, sizeof(relative_path))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid target directory");
            return ESP_FAIL;
        }
    } else {
        relative_path[0] = '\0';
    }

    if (!join_relative_path(relative_path, folder_name, target_relative, sizeof(target_relative)) ||
        !build_fs_path_from_relative(directory_path, sizeof(directory_path), target_relative)) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Path too long");
        return ESP_FAIL;
    }

    if (mkdir(directory_path, 0775) != 0) {
        ESP_LOGE(TAG, "Failed to create directory %s (%s)", directory_path, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Create folder failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "Folder created");
    return ESP_OK;
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    char requested_path[256];
    char relative_path[256];
    char filepath[512];

    if (!get_query_value_decoded(req, "path", requested_path, sizeof(requested_path)) ||
        !normalize_relative_path(requested_path, relative_path, sizeof(relative_path)) ||
        relative_path[0] == '\0' ||
        !build_fs_path_from_relative(filepath, sizeof(filepath), relative_path)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid delete path");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleting path: %s", filepath);
    if (delete_path_recursive(filepath) != ESP_OK) {
        ESP_LOGE(TAG, "Delete failed for %s (%s)", filepath, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "Deleted");
    return ESP_OK;
}

esp_err_t start_file_server(const char *base_path)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_html_get_handler, .user_ctx = NULL };
    httpd_uri_t favicon_uri = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler, .user_ctx = NULL };
    httpd_uri_t upload_uri = { .uri = "/upload", .method = HTTP_POST, .handler = upload_post_handler, .user_ctx = NULL };
    httpd_uri_t mkdir_uri = { .uri = "/mkdir", .method = HTTP_POST, .handler = mkdir_post_handler, .user_ctx = NULL };
    httpd_uri_t delete_uri = { .uri = "/delete", .method = HTTP_DELETE, .handler = delete_handler, .user_ctx = NULL };
    httpd_uri_t file_download_uri = { .uri = "/*", .method = HTTP_GET, .handler = download_get_handler, .user_ctx = NULL };

    if (!base_path) {
        ESP_LOGE(TAG, "Base path cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_server) {
        ESP_LOGW(TAG, "File server already running");
        return ESP_OK;
    }

    strlcpy(s_base_path, base_path, sizeof(s_base_path));
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;

    log_heap_state("Before file server start");
    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting server");
        return ESP_FAIL;
    }

    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &favicon_uri);
    httpd_register_uri_handler(s_server, &upload_uri);
    httpd_register_uri_handler(s_server, &mkdir_uri);
    httpd_register_uri_handler(s_server, &delete_uri);
    httpd_register_uri_handler(s_server, &file_download_uri);

    ESP_LOGI(TAG, "File Server started. Browse to the IP to manage files.");
    log_heap_state("After file server start");
    return ESP_OK;
}

esp_err_t stop_file_server(void)
{
    if (!s_server) {
        ESP_LOGW(TAG, "File server is not running");
        return ESP_OK;
    }

    log_heap_state("Before file server stop");
    if (httpd_stop(s_server) != ESP_OK) {
        ESP_LOGE(TAG, "Error stopping server");
        return ESP_FAIL;
    }

    s_server = NULL;
    ESP_LOGI(TAG, "File Server stopped");
    log_heap_state("After file server stop");
    return ESP_OK;
}