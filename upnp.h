/* upnp.h - minimal DLNA/UPnP AV MediaServer support for darkhttpd.
 *
 * This module makes a darkhttpd instance discoverable on the local network as
 * a UPnP/DLNA MediaServer.  It answers SSDP discovery requests, serves the
 * device description and service documents, and implements just enough of the
 * ContentDirectory and ConnectionManager SOAP services for a TV to browse the
 * media files served by darkhttpd and stream them over plain HTTP.
 *
 * Everything is single-threaded and driven from darkhttpd's select() loop:
 *   - upnp_ssdp_fd()      returns a socket to include in the read set (or -1)
 *   - upnp_handle_ssdp()  reads and answers pending discovery datagrams
 *   - upnp_next_timeout() seconds until the next scheduled announcement, or -1
 *   - upnp_periodic()     performs that scheduled work
 *
 * HTTP requests (device description, SCPD, SOAP control, eventing) are handed
 * to upnp_http_request(), which formats a complete HTTP response.  Media files
 * themselves are still served by darkhttpd; upnp_media_headers() supplies the
 * extra DLNA headers to attach to those responses.
 */

#ifndef DARKHTTPD_UPNP_H
#define DARKHTTPD_UPNP_H

#include <stddef.h>

/* Initialize the DLNA/UPnP subsystem.
 *
 * friendly_name is the name shown on the TV (NULL to use the hostname).
 * wwwroot is the directory being served (used as the content root).
 * http_port is the port darkhttpd's HTTP server is reachable on.
 * bind_addr is darkhttpd's --addr (may be NULL); advertise_ip overrides the
 * address advertised to clients (may be NULL).  Returns 0 on success and a
 * negative value if the subsystem could not be started. */
int upnp_init(const char *friendly_name, const char *wwwroot,
              unsigned short http_port, const char *bind_addr,
              const char *advertise_ip);

/* Announce shutdown (ssdp:byebye) and release resources. */
void upnp_shutdown(void);

/* SSDP socket for select(), or -1 when SSDP is unavailable. */
int upnp_ssdp_fd(void);

/* Read and process any pending SSDP datagrams (call when the socket is
 * readable). */
void upnp_handle_ssdp(void);

/* Seconds until the next scheduled SSDP announcement, or -1 if none. */
int upnp_next_timeout(void);

/* Perform scheduled SSDP work (periodic ssdp:alive announcements). */
void upnp_periodic(void);

/* If url names a DLNA endpoint, build a complete HTTP response and return the
 * HTTP status code, storing an allocated response in *response (caller frees)
 * and its length in *response_length.  Return 0 if the URL is not a DLNA
 * endpoint (darkhttpd then handles it normally).
 *
 * method/url/request/request_length describe the incoming request; request is
 * used to read the SOAP body of control requests. */
int upnp_http_request(const char *method, const char *url,
                      const char *request, size_t request_length,
                      char **response, size_t *response_length);

/* Extra headers to append to a media response for the given Content-Type, or
 * an empty string when none apply.  The returned string ends in CRLF. */
const char *upnp_media_headers(const char *mimetype);

#endif /* DARKHTTPD_UPNP_H */
