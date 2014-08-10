/*
 * Copyright (C) 2014 Espen Jürgensen <espenjurgensen@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#include <gcrypt.h>
#include <mxml.h>
#include <event.h>
#if defined HAVE_LIBEVENT2
# include <event2/http.h>
#else
# include "evhttp/evhttp_compat.h"
#endif

#include <sys/socket.h>
#include <netinet/in.h>

#include "lastfm.h"
#include "logger.h"
#include "misc.h"

// For http_make_request
#include <event2/bufferevent_ssl.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/http.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>


enum lastfm_state
{
  LASTFM_STATE_UNKNOWN,
  LASTFM_STATE_INACTIVE,
  LASTFM_STATE_ACTIVE,
};

struct lastfm_command;

typedef int (*cmd_func)(struct lastfm_command *cmd);

struct lastfm_command
{
  pthread_mutex_t lck;
  pthread_cond_t cond;

  cmd_func func;

  int nonblock;

  union {
    void *noarg;
    struct media_file_info mfi;
  } arg;

  int ret;
};

struct param_t {
  char *key;
  char *val;
  struct param_t *next;
  struct param_t *tmp;
};


/* --- Globals --- */
// lastfm thread
static pthread_t tid_lastfm;

// Event base, pipes and events
struct event_base *evbase_lastfm;
static int g_exit_pipe[2];
static int g_cmd_pipe[2];
static struct event *g_exitev;
static struct event *g_cmdev;

// The global state telling us what the thread is currently doing
static enum lastfm_state g_state = LASTFM_STATE_UNKNOWN;

/**
 * The API key and secret (not so secret being open source) is specific to 
 * forked-daapd, and is used to identify forked-daapd and to sign requests
 */
const char *g_api_key = "579593f2ed3f49673c7364fd1c9c829b";
const char *g_secret = "ce45a1d275c10b3edf0ecfa27791cb2b";

const char *api_url = "http://ws.audioscrobbler.com/2.0/";
const char *auth_url = "https://ws.audioscrobbler.com/2.0/";
//const char *auth_url = "http://192.168.1.1/";

// Session key
char *g_session_key;


/* ------------------------------ HTTPS CLIENT ----------------------------- */
/*           Also supports http. TODO: Host certificate validation           */

static void
https_request_cb(struct evhttp_request *req, void *ctx)
{
  struct evbuffer *input_buffer;
  mxml_node_t *tree;
  mxml_node_t *s_node;
  mxml_node_t *e_node;
  char buffer[256];
  char *body;
  char *errmsg;
  char *sk;
  int response_code;


  if (req == NULL) {
    /* If req is NULL, it means an error occurred, but
     * sadly we are mostly left guessing what the error
     * might have been.  We'll do our best... */
    struct bufferevent *bev = (struct bufferevent *) ctx;
    unsigned long oslerr;
    int printed_err = 0;
    int errcode = EVUTIL_SOCKET_ERROR();

    DPRINTF(E_LOG, L_LASTFM, "Request failed\n");
    /* Print out the OpenSSL error queue that libevent
     * squirreled away for us, if any. */
    while ((oslerr = bufferevent_get_openssl_error(bev))) {
      ERR_error_string_n(oslerr, buffer, sizeof(buffer));
      DPRINTF(E_LOG, L_LASTFM, "- error message: %s\n", buffer);
      printed_err = 1;
    }
    /* If the OpenSSL error queue was empty, maybe it was a
     * socket error; let's try printing that. */
    if (! printed_err)
      DPRINTF(E_LOG, L_LASTFM, "- socket error: %s (%d)\n", evutil_socket_error_to_string(errcode), errcode);
    return;
  }

  // Load response content
  input_buffer = evhttp_request_get_input_buffer(req);
  evbuffer_add(input_buffer, "", 1); // NULL-terminate the buffer

  body = (char *)evbuffer_pullup(input_buffer, -1);
  if (!body || (strlen(body) == 0))
    {
      DPRINTF(E_LOG, L_LASTFM, "Empty response\n");
      return;
    }

  tree = mxmlLoadString(NULL, body, MXML_OPAQUE_CALLBACK);
  if (!tree)
    return;

  // Look for errors
  response_code = evhttp_request_get_response_code(req);
  e_node = mxmlFindPath(tree, "lfm/error");
  if (e_node)
    {
      errmsg = trimwhitespace(mxmlGetOpaque(e_node));
      DPRINTF(E_LOG, L_LASTFM, "Request to LastFM failed (HTTP error %d): %s\n", response_code, errmsg);

      if (errmsg)
	free(errmsg);
      mxmlDelete(tree);
      return;
    }
  if (response_code != HTTP_OK)
    {
      DPRINTF(E_LOG, L_LASTFM, "Request to LastFM failed (HTTP error %d): No LastFM error description\n", response_code);
      mxmlDelete(tree);
      return;
    }

  // Get the session key
  s_node = mxmlFindPath(tree, "lfm/session/key");
  if (!s_node)
    {
      DPRINTF(E_LOG, L_LASTFM, "Session key not found\n");
      mxmlDelete(tree);
      return;
    }

  sk = trimwhitespace(mxmlGetOpaque(s_node));
  DPRINTF(E_LOG, L_LASTFM, "Got session key (%s) from LastFM\n", sk);

  if (sk)
    free(sk);
  mxmlDelete(tree);
}

static int
https_request(const char *url, const char *body, char **errmsg)
{
  struct event_base *base;
  struct evhttp_uri *http_uri;
  struct bufferevent *bev;
  struct evhttp_connection *evcon;
  struct evhttp_request *req;
  struct evkeyvalq *output_headers;
  struct evbuffer *output_buffer;
  const char *scheme, *host, *path, *query;
  char uri[256];
  char content_len[8];
  int port;
  int r;
  SSL_CTX *ssl_ctx;
  SSL *ssl;

  *errmsg = NULL;

  http_uri = evhttp_uri_parse(url);
  if (http_uri == NULL) {
    *errmsg = "Malformed url";
    return -1;
  }

  scheme = evhttp_uri_get_scheme(http_uri);
  if (scheme == NULL || (strcasecmp(scheme, "https") != 0 &&
                         strcasecmp(scheme, "http") != 0)) {
    *errmsg = "URL must be http or https";
    return -1;
  }

  host = evhttp_uri_get_host(http_uri);
  if (host == NULL) {
    *errmsg = "URL must have a host";
    return -1;
  }

  port = evhttp_uri_get_port(http_uri);
  if (port == -1) {
    port = (strcasecmp(scheme, "http") == 0) ? 80 : 443;
  }

  path = evhttp_uri_get_path(http_uri);
  if (path == NULL) {
    path = "/";
  }

  query = evhttp_uri_get_query(http_uri);
  if (query == NULL) {
    snprintf(uri, sizeof(uri) - 1, "%s", path);
  } else {
    snprintf(uri, sizeof(uri) - 1, "%s?%s", path, query);
  }
  uri[sizeof(uri) - 1] = '\0';

  // Initialize OpenSSL
  SSL_library_init();
  ERR_load_crypto_strings();
  SSL_load_error_strings();
  OpenSSL_add_all_algorithms();

  /* Create a new OpenSSL context */
  ssl_ctx = SSL_CTX_new(SSLv23_method());
  if (!ssl_ctx) {
    *errmsg = "Could not create SSL context";
    return -1;
  }

  // Create event base
  base = event_base_new();
  if (!base) {
    *errmsg = "Could not create event base";
    return -1;
  }

  // Point global to the base
  evbase_lastfm = base;

  // Create OpenSSL bufferevent and stack evhttp on top of it
  ssl = SSL_new(ssl_ctx);
  if (ssl == NULL) {
    *errmsg = "Could not create SSL bufferevent";
    event_base_free(base);
    return -1;
  }

  // Set hostname for SNI extension
  SSL_set_tlsext_host_name(ssl, host);

  if (strcasecmp(scheme, "http") == 0) {
    bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
  } else {
    bev = bufferevent_openssl_socket_new(base, -1, ssl,
      BUFFEREVENT_SSL_CONNECTING,
      BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);
  }

  if (bev == NULL) {
    *errmsg = "Could not create bufferevent";
    event_base_free(base);
    return -1;
  }

  bufferevent_openssl_set_allow_dirty_shutdown(bev, 1);

  evcon = evhttp_connection_base_bufferevent_new(base, NULL, bev, host, port);
  if (evcon == NULL) {
    *errmsg = "Could not create evcon";
    event_base_free(base);
    return -1;
  }

  // Fire off the request
  req = evhttp_request_new(https_request_cb, bev);
  if (req == NULL) {
    *errmsg = "Could not create request";
    evhttp_connection_free(evcon);
    event_base_free(base);
    return -1;
  }

  output_headers = evhttp_request_get_output_headers(req);
  evhttp_add_header(output_headers, "Host", host);
  evhttp_add_header(output_headers, "Connection", "close");
  evhttp_add_header(output_headers, "User-Agent", "forked-daapd");
  evhttp_add_header(output_headers, "Accept-Charset", "utf-8");
  evhttp_add_header(output_headers, "Content-Type", "application/x-www-form-urlencoded");

  if (body) {
    output_buffer = evhttp_request_get_output_buffer(req);
    evbuffer_add(output_buffer, body, strlen(body));
    snprintf(content_len, sizeof(content_len), "%d", strlen(body));
    evhttp_add_header(output_headers, "Content-Length", content_len);
  }

  r = evhttp_make_request(evcon, req, body ? EVHTTP_REQ_POST : EVHTTP_REQ_GET, uri);
  if (r != 0) {
    *errmsg = "Could not make request";
    evhttp_connection_free(evcon);
    event_base_free(base);
    return -1;
  }

  event_base_dispatch(base);

  evhttp_connection_free(evcon);
  event_base_free(base);

  return 0;
}


/* --------------------------------- HELPERS ------------------------------- */

static int
param_add(struct param_t **param, const char *key, const char *val)
{
  struct param_t *new;

  new = (struct param_t *)malloc(sizeof(struct param_t));
  if (!new)
    {
      DPRINTF(E_LOG, L_LASTFM, "Out of memory adding new parameter: %s\n", key);
      return -1;
    }

  new->key  = strdup(key);
  new->val  = strdup(val);
  new->next = *param;

  *param = new;

  return 0;
}

static void
param_sort(struct param_t **param)
{
  struct param_t *sorted;
  struct param_t *p;
  struct param_t *s;

  sorted = *param;
  for (p = *param; p != NULL; p = p->next)
    {
//      DPRINTF(E_DBG, L_LASTFM, "Finding next for %s\n", p->key);
      p->tmp = NULL;
      for (s = *param; s != NULL; s = s->next)
	{
	  // We try to find a key in param which is greater than p->key
	  // but less than our current candidate (p->tmp->key)
	  if ( (strcmp(s->key, p->key) > 0) &&
	       ((p->tmp == NULL) || (strcmp(s->key, p->tmp->key) < 0)) )
	    p->tmp = s;
	}
/*if (p->tmp)
      DPRINTF(E_DBG, L_LASTFM, "Next for %s is %s\n", p->key, p->tmp->key);
else
      DPRINTF(E_DBG, L_LASTFM, "No next for %s\n", p->key);
*/
      // Find smallest key, which will be the new head
      if (strcmp(p->key, sorted->key) < 0)
	sorted = p;
    }

//  DPRINTF(E_DBG, L_LASTFM, "Setting new next\n");
  while ((p = *param))
    {
      *param  = p->next;
      p->next = p->tmp;
    }

//  DPRINTF(E_DBG, L_LASTFM, "Setting param\n");
  *param = sorted;

//  DPRINTF(E_DBG, L_LASTFM, "Sorted request param: %s\n", sorted->key);
}

/* Converts parameters to a string in application/x-www-form-urlencoded format */
static int
param_print(char **body, struct param_t *param)
{
  struct evbuffer *evbuf;
  struct param_t *p;
  char *k;
  char *v;

  evbuf = evbuffer_new();

  for (p = param; p != NULL; p = p->next)
    {
      k = evhttp_encode_uri(p->key);
      if (!k)
        continue;

      v = evhttp_encode_uri(p->val);
      if (!v)
	{
	  free(k);
	  continue;
	}

      evbuffer_add(evbuf, k, strlen(k));
      evbuffer_add(evbuf, "=", 1);
      evbuffer_add(evbuf, v, strlen(v));
      if (p->next)
	evbuffer_add(evbuf, "&", 1);

      free(k);
      free(v);
    }

  evbuffer_add(evbuf, "\n", 1);

  *body = evbuffer_readln(evbuf, NULL, EVBUFFER_EOL_ANY);

  evbuffer_free(evbuf);

  DPRINTF(E_DBG, L_LASTFM, "Parameters in request are: %s\n", *body);

  return 0;
}

static void
param_free(struct param_t *param)
{
  struct param_t *p;

  while ((p = param))
    {
      param = p->next;
      free(p->key);
      free(p->val);
      free(p);
    }
}

/* Creates an md5 signature of the concatenated parameters and adds it to param */
static int
lastfm_sign(struct param_t **param)
{
  struct param_t *p;

  char hash[33];
  char ebuf[64];
  uint8_t *hash_bytes;
  size_t hash_len;
  gcry_md_hd_t md_hdl;
  gpg_error_t gc_err;
  int ret;
  int i;

  gc_err = gcry_md_open(&md_hdl, GCRY_MD_MD5, 0);
  if (gc_err != GPG_ERR_NO_ERROR)
    {
      gpg_strerror_r(gc_err, ebuf, sizeof(ebuf));
      DPRINTF(E_LOG, L_LASTFM, "Could not open MD5: %s\n", ebuf);
      return -1;
    }

  for (p = *param; p != NULL; p = p->next)
    {
      gcry_md_write(md_hdl, p->key, strlen(p->key));
      gcry_md_write(md_hdl, p->val, strlen(p->val));
    }  

  gcry_md_write(md_hdl, g_secret, strlen(g_secret));

  hash_bytes = gcry_md_read(md_hdl, GCRY_MD_MD5);
  if (!hash_bytes)
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not read MD5 hash\n");
      return -1;
    }

  hash_len = gcry_md_get_algo_dlen(GCRY_MD_MD5);

  for (i = 0; i < hash_len; i++)
    sprintf(hash + (2 * i), "%02x", hash_bytes[i]);

  ret = param_add(param, "api_sig", hash);

  gcry_md_close(md_hdl);

  return ret;
}

static int
lastfm_request_post(char *method, struct param_t **param, int auth)
{
  char *errmsg;
  char *body;
  int ret;

  ret = param_add(param, "method", method);
  if (ret < 0)
    return -1;

  if (!auth)
    ret = param_add(param, "sk", g_session_key);
  if (ret < 0)
    return -1;

  // API requires that we MD5 sign sorted param (without "format" param)
  param_sort(param);
  ret = lastfm_sign(param);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_LASTFM, "Aborting request, lastfm_sign failed\n");
      return -1;
    }

  if (!auth)
    param_add(param, "format", "json");
  if (ret < 0)
    return -1;

  ret = param_print(&body, *param);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_LASTFM, "Aborting request, param_print failed\n");
      return -1;
    }

  param_free(*param);

  if (!auth)
    ret = https_request(api_url, body, &errmsg);
  else
    ret = https_request(auth_url, body, &errmsg);

  if (ret < 0)
    DPRINTF(E_LOG, L_LASTFM, "Request failed: %s\n", errmsg);

  return ret;
}

static int
scrobble(struct lastfm_command *cmd)
{
  return 0;
}

static int
lastfm_file_read(char *path, char **username, char **password)
{
  FILE *fp;
  char *u;
  char *p;
  char buf[256];
  int len;

  fp = fopen(path, "rb");
  if (!fp)
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not open lastfm credentials file %s: %s\n", path, strerror(errno));
      return -1;
    }

  u = fgets(buf, sizeof(buf), fp);
  if (!u)
    {
      DPRINTF(E_LOG, L_LASTFM, "Empty lastfm credentials file %s\n", path);

      fclose(fp);
      return -1;
    }

  len = strlen(u);
  if (buf[len - 1] != '\n')
    {
      DPRINTF(E_LOG, L_LASTFM, "Invalid lastfm credentials file %s: username name too long or missing password\n", path);

      fclose(fp);
      return -1;
    }

  while (len)
    {
      if ((buf[len - 1] == '\r') || (buf[len - 1] == '\n'))
	{
	  buf[len - 1] = '\0';
	  len--;
	}
      else
	break;
    }

  if (!len)
    {
      DPRINTF(E_LOG, L_LASTFM, "Invalid lastfm credentials file %s: empty line where username expected\n", path);

      fclose(fp);
      return -1;
    }

  u = strdup(buf);
  if (!u)
    {
      DPRINTF(E_LOG, L_LASTFM, "Out of memory for username while reading %s\n", path);

      fclose(fp);
      return -1;
    }

  p = fgets(buf, sizeof(buf), fp);
  fclose(fp);
  if (!p)
    {
      DPRINTF(E_LOG, L_LASTFM, "Invalid lastfm credentials file %s: no password\n", path);

      free(u);
      return -1;
    }

  len = strlen(p);

  while (len)
    {
      if ((buf[len - 1] == '\r') || (buf[len - 1] == '\n'))
	{
	  buf[len - 1] = '\0';
	  len--;
	}
      else
	break;
    }

  p = strdup(buf);
  if (!p)
    {
      DPRINTF(E_LOG, L_LASTFM, "Out of memory for password while reading %s\n", path);

      free(u);
      return -1;
    }

  DPRINTF(E_LOG, L_LASTFM, "lastfm credentials file OK, logging in with username %s\n", u);

  *username = u;
  *password = p;

  return 0;
}


/* ---------------------------- COMMAND EXECUTION -------------------------- */

static int
send_command(struct lastfm_command *cmd)
{
  int ret;

  if (!cmd->func)
    {
      DPRINTF(E_LOG, L_LASTFM, "BUG: cmd->func is NULL!\n");
      return -1;
    }

  ret = write(g_cmd_pipe[1], &cmd, sizeof(cmd));
  if (ret != sizeof(cmd))
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not send command: %s\n", strerror(errno));
      return -1;
    }

  return 0;
}

static int
nonblock_command(struct lastfm_command *cmd)
{
  int ret;

  ret = send_command(cmd);
  if (ret < 0)
    return -1;

  return 0;
}

/* Thread: main */
static void
thread_exit(void)
{
  int dummy = 42;

  DPRINTF(E_DBG, L_LASTFM, "Killing lastfm thread\n");

  if (write(g_exit_pipe[1], &dummy, sizeof(dummy)) != sizeof(dummy))
    DPRINTF(E_LOG, L_LASTFM, "Could not write to exit fd: %s\n", strerror(errno));
}



/* ------------------------------- MAIN LOOP ------------------------------- */
/*                              Thread: lastfm                              */

static void *
lastfm(void *arg)
{
  DPRINTF(E_DBG, L_LASTFM, "Main loop initiating\n");

  g_state = LASTFM_STATE_ACTIVE;

  event_base_dispatch(evbase_lastfm);

  if (g_state != LASTFM_STATE_INACTIVE)
    {
      DPRINTF(E_LOG, L_LASTFM, "lastfm event loop terminated ahead of time!\n");
      g_state = LASTFM_STATE_INACTIVE;
    }

  DPRINTF(E_DBG, L_LASTFM, "Main loop terminating\n");

  pthread_exit(NULL);
}

static void
exit_cb(int fd, short what, void *arg)
{
  int dummy;
  int ret;

  ret = read(g_exit_pipe[0], &dummy, sizeof(dummy));
  if (ret != sizeof(dummy))
    DPRINTF(E_LOG, L_LASTFM, "Error reading from exit pipe\n");

  event_base_loopbreak(evbase_lastfm);

  g_state = LASTFM_STATE_INACTIVE;

  event_add(g_exitev, NULL);
}

static void
command_cb(int fd, short what, void *arg)
{
  struct lastfm_command *cmd;
  int ret;

  ret = read(g_cmd_pipe[0], &cmd, sizeof(cmd));
  if (ret != sizeof(cmd))
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not read command! (read %d): %s\n", ret, (ret < 0) ? strerror(errno) : "-no error-");
      goto readd;
    }

  if (cmd->nonblock)
    {
      cmd->func(cmd);

      free(cmd);
      goto readd;
    }

  pthread_mutex_lock(&cmd->lck);

  ret = cmd->func(cmd);
  cmd->ret = ret;

  pthread_cond_signal(&cmd->cond);
  pthread_mutex_unlock(&cmd->lck);

 readd:
  event_add(g_cmdev, NULL);
}


/* ---------------------------- Our lastfm API  --------------------------- */

static int
lastfm_init(void);

/* Thread: filescanner */
void
lastfm_login(char *path)
{
  struct param_t *param;
  char *username;
  char *password;
  int ret;

//  db_lastfm_session_delete();

  ret = lastfm_file_read(path, &username, &password);
  if (ret < 0)
    return;

  // Spawn thread
//  lastfm_init();

  param = NULL;
  param_add(&param, "api_key", g_api_key);
  param_add(&param, "username", username);
  param_add(&param, "password", password);

  // Send an authentication request and wait for callback
  lastfm_request_post("auth.getMobileSession", &param, 1);
}

/* Thread: http and player */
int
lastfm_scrobble(struct media_file_info *mfi)
{
  struct lastfm_command *cmd;

  // User is not using LastFM (no valid session key is available)
  if (g_state == LASTFM_STATE_INACTIVE)
    return -1;

  // Don't scrobble songs which are shorter than 30 sec
  if (mfi->song_length < 30000)
    return -1;

  // Don't scrobble songs with unknown artist
  if (strcmp(mfi->artist, "Unknown artist") == 0)
    return -1;

  // First time lastfm_scrobble is called the state will be LASTFM_UNKNOWN
//  if (g_state == LASTFM_STATE_UNKNOWN)
//    db_lastfm_session_get(&g_session_key);

  if (!g_session_key)
    {
      DPRINTF(E_INFO, L_LASTFM, "No valid LastFM session key\n");
      g_state = LASTFM_STATE_INACTIVE;
      return -1;
    }

  // Spawn LastFM thread
  lastfm_init();

  // Send scrobble command to the thread
  DPRINTF(E_DBG, L_LASTFM, "LastFM scrobble request\n");

  cmd = (struct lastfm_command *)malloc(sizeof(struct lastfm_command));
  if (!cmd)
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not allocate lastfm_command\n");
      return -1;
    }

  memset(cmd, 0, sizeof(struct lastfm_command));

  cmd->nonblock = 1;

  cmd->func = scrobble;
  cmd->arg.mfi.artist = strdup(mfi->artist);

  nonblock_command(cmd);

  return 0;
}

static int
lastfm_init(void)
{
  int ret;

  if (g_state == LASTFM_STATE_ACTIVE)
    return -1;

# if defined(__linux__)
  ret = pipe2(g_exit_pipe, O_CLOEXEC);
# else
  ret = pipe(g_exit_pipe);
# endif
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not create pipe: %s\n", strerror(errno));
      goto exit_fail;
    }

# if defined(__linux__)
  ret = pipe2(g_cmd_pipe, O_CLOEXEC);
# else
  ret = pipe(g_cmd_pipe);
# endif
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not create command pipe: %s\n", strerror(errno));
      goto cmd_fail;
    }

  evbase_lastfm = event_base_new();
  if (!evbase_lastfm)
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not create an event base\n");
      goto evbase_fail;
    }

#ifdef HAVE_LIBEVENT2
  g_exitev = event_new(evbase_lastfm, g_exit_pipe[0], EV_READ, exit_cb, NULL);
  if (!g_exitev)
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not create exit event\n");
      goto evnew_fail;
    }

  g_cmdev = event_new(evbase_lastfm, g_cmd_pipe[0], EV_READ, command_cb, NULL);
  if (!g_cmdev)
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not create cmd event\n");
      goto evnew_fail;
    }
#else
  g_exitev = (struct event *)malloc(sizeof(struct event));
  if (!g_exitev)
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not create exit event\n");
      goto evnew_fail;
    }
  event_set(g_exitev, g_exit_pipe[0], EV_READ, exit_cb, NULL);
  event_base_set(evbase_lastfm, g_exitev);

  g_cmdev = (struct event *)malloc(sizeof(struct event));
  if (!g_cmdev)
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not create cmd event\n");
      goto evnew_fail;
    }
  event_set(g_cmdev, g_cmd_pipe[0], EV_READ, command_cb, NULL);
  event_base_set(evbase_lastfm, g_cmdev);
#endif

  event_add(g_exitev, NULL);
  event_add(g_cmdev, NULL);

  DPRINTF(E_INFO, L_LASTFM, "LastFM thread init\n");

  ret = pthread_create(&tid_lastfm, NULL, lastfm, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_LASTFM, "Could not spawn LastFM thread: %s\n", strerror(errno));

      goto thread_fail;
    }

  return 0;
  
 thread_fail:
 evnew_fail:
  event_base_free(evbase_lastfm);
  evbase_lastfm = NULL;

 evbase_fail:
  close(g_cmd_pipe[0]);
  close(g_cmd_pipe[1]);

 cmd_fail:
  close(g_exit_pipe[0]);
  close(g_exit_pipe[1]);

 exit_fail:
  return -1;
}

static void
lastfm_deinit(void)
{
  int ret;

  /* Send exit signal to thread (if active) */
  if (g_state == LASTFM_STATE_ACTIVE)
    {
      thread_exit();

      ret = pthread_join(tid_lastfm, NULL);
      if (ret != 0)
	{
	  DPRINTF(E_FATAL, L_LASTFM, "Could not join lastfm thread: %s\n", strerror(errno));
	  return;
	}
    }

  /* Free event base (should free events too) */
  event_base_free(evbase_lastfm);

  /* Close pipes */
  close(g_cmd_pipe[0]);
  close(g_cmd_pipe[1]);
  close(g_exit_pipe[0]);
  close(g_exit_pipe[1]);
}