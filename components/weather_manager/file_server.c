#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_http_server.h"
#include "file_server.h"

static const char *TAG = "file_server";
static char s_base_path[ESP_VFS_PATH_MAX + 1];

/* HTML content for the client */
static const char* HTML_INDEX_HEADER = 
    "<!DOCTYPE html><html><head><title>KidBox Manager</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>"
    "body { font-family: sans-serif; background: #222; color: #fff; padding: 20px; }"
    "table { width: 100%; border-collapse: collapse; margin-top: 20px; }"
    "th, td { border: 1px solid #444; padding: 8px; text-align: left; }"
    "a { color: #4af; text-decoration: none; }"
    ".btn { background: #d33; color: white; padding: 5px 10px; border: none; cursor: pointer; }"
    ".upload-box { background: #333; padding: 20px; border: 2px dashed #555; text-align: center; }"
    "</style>"
    "<script>"
    "function upload(file) {"
    "  var status = document.getElementById('status');"
    "  status.innerText = 'Uploading ' + file.name + '...';"
    "  fetch('/upload/' + file.name, { method: 'POST', body: file }).then(resp => {"
    "    if (resp.ok) { status.innerText = 'Done!'; location.reload(); }"
    "    else { status.innerText = 'Failed!'; }"
    "  });"
    "}"
    "function del(path) {"
    "  if(confirm('Delete ' + path + '?')) {"
    "    fetch('/delete' + path, { method: 'DELETE' }).then(resp => location.reload());"
    "  }"
    "}"
    "</script>"
    "</head><body>"
    "<h2>KidBox SD Card</h2>"
    "<div class='upload-box'>"
    "  <input type='file' id='file_input' onchange='upload(this.files[0])'>"
    "  <p id='status'>Select a file to upload</p>"
    "</div>"
    "<table><tr><th>Name</th><th>Size</th><th>Action</th></tr>";

static const char* HTML_INDEX_FOOTER = "</table></body></html>";

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

/* Handler to download a file */
static esp_err_t download_get_handler(httpd_req_t *req)
{
    char filepath[512];
    FILE *fd = NULL;
    struct stat file_stat;

    // Skip the leading slash
    const char *filename = req->uri;
    
    // Construct full path
    if (!build_path(filepath, sizeof(filepath), s_base_path, filename, false)) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Path too long");
        return ESP_FAIL;
    }

    if (stat(filepath, &file_stat) == -1) {
        ESP_LOGE(TAG, "File not found : %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    fd = fopen(filepath, "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sending file : %s (%ld bytes)", filename, file_stat.st_size);
    httpd_resp_set_type(req, "application/octet-stream"); // Default binary type

    // Use a simpler chunk size to avoid stack overflow
    char *chunk = malloc(4096);
    if (chunk) {
        size_t chunksize;
        do {
            chunksize = fread(chunk, 1, 4096, fd);
            if (chunksize > 0) {
                if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
                    fclose(fd);
                    free(chunk);
                    ESP_LOGE(TAG, "File sending failed!");
                    return ESP_FAIL;
                }
            }
        } while (chunksize != 0);
        free(chunk);
    }
    fclose(fd);
    httpd_resp_send_chunk(req, NULL, 0); // End response
    return ESP_OK;
}

/* Handler to list files (Index) */
static esp_err_t index_html_get_handler(httpd_req_t *req)
{
    httpd_resp_sendstr_chunk(req, HTML_INDEX_HEADER);

    DIR *dir = opendir(s_base_path);
    char entrypath[512];
    char entrysize[16];
    struct dirent *entry;
    struct stat entry_stat;

    if (dir) {
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_DIR) continue; // Skip directories for simplicity

            if (!build_path(entrypath, sizeof(entrypath), s_base_path, entry->d_name, true)) {
                continue;
            }
            if (stat(entrypath, &entry_stat) == -1) {
                strcpy(entrysize, "?");
            } else {
                snprintf(entrysize, sizeof(entrysize), "%ld", entry_stat.st_size);
            }

            httpd_resp_sendstr_chunk(req, "<tr><td><a href='");
            httpd_resp_sendstr_chunk(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "'>");
            httpd_resp_sendstr_chunk(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "</a></td><td>");
            httpd_resp_sendstr_chunk(req, entrysize);
            httpd_resp_sendstr_chunk(req, "</td><td><button class='btn' onclick=\"del('/");
            httpd_resp_sendstr_chunk(req, entry->d_name);
            httpd_resp_sendstr_chunk(req, "')\">Del</button></td></tr>");
        }
        closedir(dir);
    }

    httpd_resp_sendstr_chunk(req, HTML_INDEX_FOOTER);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Handler to upload a file */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char filepath[512];
    FILE *fd = NULL;
    
    // URI is /upload/filename.png
    const char *filename = req->uri + strlen("/upload");
    if (strlen(filename) == 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename missing");
        return ESP_FAIL;
    }

    if (!build_path(filepath, sizeof(filepath), s_base_path, filename, false)) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Path too long");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Receiving file: %s", filepath);

    fd = fopen(filepath, "w");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to create file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    int received;

    while ((received = httpd_req_recv(req, buf, 4096)) > 0) {
        if (fwrite(buf, 1, received, fd) != received) {
            ESP_LOGE(TAG, "File write failed");
            fclose(fd);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "File write failed");
            return ESP_FAIL;
        }
    }
    free(buf);
    fclose(fd);

    httpd_resp_sendstr(req, "File uploaded successfully");
    return ESP_OK;
}

/* Handler to delete a file */
static esp_err_t delete_handler(httpd_req_t *req)
{
    char filepath[512];
    // URI is /delete/filename.png
    const char *filename = req->uri + strlen("/delete");
    
    if (!build_path(filepath, sizeof(filepath), s_base_path, filename, false)) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Path too long");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Deleting file: %s", filepath);

    if (unlink(filepath) == 0) {
        httpd_resp_sendstr(req, "File deleted");
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
    return ESP_FAIL;
}

esp_err_t start_file_server(const char *base_path)
{
    if (!base_path) {
        ESP_LOGE(TAG, "Base path cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_base_path, base_path, sizeof(s_base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192; // Increase stack for file ops

    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting server!");
        return ESP_FAIL;
    }

    // 1. GET / -> Index
    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_html_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &index_uri);

    // 2. POST /upload/* -> Upload
    httpd_uri_t upload_uri = { .uri = "/upload/*", .method = HTTP_POST, .handler = upload_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &upload_uri);

    // 3. DELETE /delete/* -> Delete
    httpd_uri_t delete_uri = { .uri = "/delete/*", .method = HTTP_DELETE, .handler = delete_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &delete_uri);

    // 4. GET /* -> Download (Must be last to match everything else)
    httpd_uri_t file_download_uri = { .uri = "/*", .method = HTTP_GET, .handler = download_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &file_download_uri);

    ESP_LOGI(TAG, "File Server started. Browse to the IP to manage files.");
    return ESP_OK;
}