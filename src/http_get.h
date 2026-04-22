#ifndef HTTP_GET_H
#define HTTP_GET_H

#ifdef __cplusplus
extern "C" {
#endif

// Download an https:// URL and write the HTTP response body to dest_path on SD.
// progress_cb: optional callback called after each chunk — args are (bytes_written, content_length).
//              content_length is -1 when the server omits Content-Length.
// Returns 0 on success, -1 on error.
// Requires nina_init() and a successful nina_connect_wpa() before calling.
int http_get_to_file(const char *url, const char *dest_path,
                     void (*progress_cb)(int bytes_written, int content_length));

#ifdef __cplusplus
}
#endif

#endif // HTTP_GET_H
