#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "finwo/pbkdf2.h"
#include "rxi/log.h"

#include "util/auth.h"

#define HASH_LEN 32
#define SALT_LEN 16

static const char AB64[] =
  "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

int ab64_encode(const unsigned char *in, size_t len, char *out, size_t out_sz) {
  size_t need = (len * 8 + 5) / 6;
  if (out_sz < need + 1) return -1;

  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    unsigned v = (unsigned)in[i] << 16;
    if (i + 1 < len) v |= (unsigned)in[i + 1] << 8;
    if (i + 2 < len) v |= (unsigned)in[i + 2];

    out[o++] = AB64[(v >> 18) & 0x3f];
    out[o++] = AB64[(v >> 12) & 0x3f];
    if (i + 1 < len) out[o++] = AB64[(v >> 6) & 0x3f];
    if (i + 2 < len) out[o++] = AB64[v & 0x3f];
  }
  out[o] = '\0';
  return 0;
}

static int ab64_val(char c) {
  const char *p = strchr(AB64, c);
  return (p && c) ? (int)(p - AB64) : -1;
}

size_t ab64_decode(const char *in, unsigned char *out, size_t out_sz) {
  size_t len = strlen(in);
  size_t o   = 0;

  for (size_t i = 0; i < len; i += 4) {
    int c0 = ab64_val(in[i]);
    int c1 = (i + 1 < len) ? ab64_val(in[i + 1]) : -1;
    int c2 = (i + 2 < len) ? ab64_val(in[i + 2]) : -1;
    int c3 = (i + 3 < len) ? ab64_val(in[i + 3]) : -1;
    if (c0 < 0 || c1 < 0) break;

    unsigned v = ((unsigned)c0 << 18) | ((unsigned)c1 << 12);
    if (c2 >= 0) v |= (unsigned)c2 << 6;
    if (c3 >= 0) v |= (unsigned)c3;

    if (o < out_sz) out[o++] = (unsigned char)((v >> 16) & 0xff);
    if (c2 >= 0 && o < out_sz) out[o++] = (unsigned char)((v >> 8) & 0xff);
    if (c3 >= 0 && o < out_sz) out[o++] = (unsigned char)(v & 0xff);
  }
  return o;
}

// Comparison time must not depend on how much of the hash matched.
static int ct_equal(const unsigned char *a, const unsigned char *b, size_t n) {
  unsigned char diff = 0;
  for (size_t i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
  return diff == 0;
}

int auth_verify_hash(const char *stored, const char *pass) {
  if (!stored || !pass) return 0;

  const char *prefix = "$pbkdf2-sha256$";
  if (strncmp(stored, prefix, strlen(prefix)) != 0) return 0;

  char *copy = strdup(stored + strlen(prefix));
  if (!copy) return 0;

  char *iter_s = copy;
  char *salt_s = strchr(iter_s, '$');
  if (!salt_s) { free(copy); return 0; }
  *salt_s++ = '\0';
  char *hash_s = strchr(salt_s, '$');
  if (!hash_s) { free(copy); return 0; }
  *hash_s++ = '\0';

  char *end = NULL;
  unsigned long iterations = strtoul(iter_s, &end, 10);
  if (!end || *end || iterations == 0) { free(copy); return 0; }

  unsigned char salt[64], want[HASH_LEN], got[HASH_LEN];
  size_t salt_len = ab64_decode(salt_s, salt, sizeof(salt));
  size_t want_len = ab64_decode(hash_s, want, sizeof(want));
  if (salt_len == 0 || want_len != HASH_LEN) { free(copy); return 0; }

  pbkdf2((const uint8_t *)pass, strlen(pass), salt, salt_len,
         iterations, PBKDF2_SHA256, got, HASH_LEN);

  int ok = ct_equal(got, want, HASH_LEN);
  free(copy);
  return ok;
}

char *auth_make_hash(const char *pass, unsigned iterations) {
  if (!pass) return NULL;
  if (!iterations) iterations = AUTH_DEFAULT_ITERATIONS;

  unsigned char salt[SALT_LEN];
  FILE *rnd = fopen("/dev/urandom", "rb");
  if (!rnd) return NULL;
  size_t got = fread(salt, 1, sizeof(salt), rnd);
  fclose(rnd);
  if (got != sizeof(salt)) return NULL;

  unsigned char hash[HASH_LEN];
  pbkdf2((const uint8_t *)pass, strlen(pass), salt, sizeof(salt),
         iterations, PBKDF2_SHA256, hash, sizeof(hash));

  char salt_b[64], hash_b[64];
  if (ab64_encode(salt, sizeof(salt), salt_b, sizeof(salt_b)) != 0) return NULL;
  if (ab64_encode(hash, sizeof(hash), hash_b, sizeof(hash_b)) != 0) return NULL;

  char *out = NULL;
  if (asprintf(&out, "$pbkdf2-sha256$%u$%s$%s", iterations, salt_b, hash_b) < 0) {
    return NULL;
  }
  return out;
}

int auth_check_permissions(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;

  if (st.st_mode & S_IWOTH) {
    log_error("auth: %s is world-writable; refusing to use it", path);
    return -1;
  }
  if (st.st_mode & (S_IRGRP | S_IROTH)) {
    log_warn("auth: %s is readable beyond its owner", path);
  }
  return 0;
}

static int role_from_string(const char *s) {
  if (!s || !*s)                return AUTH_ROLE_READONLY;
  if (!strcasecmp(s, "full"))   return AUTH_ROLE_FULL;
  if (!strcasecmp(s, "readonly")) return AUTH_ROLE_READONLY;
  log_warn("auth: unknown role `%s', treating as readonly", s);
  return AUTH_ROLE_READONLY;
}

int auth_check(const char *path, const char *user, const char *pass) {
  if (!path || !user || !pass) return AUTH_ROLE_NONE;
  if (auth_check_permissions(path) != 0) return AUTH_ROLE_NONE;

  FILE *fd = fopen(path, "r");
  if (!fd) {
    log_error("auth: cannot open %s", path);
    return AUTH_ROLE_NONE;
  }

  char line[1024];
  int  role = AUTH_ROLE_NONE;

  while (fgets(line, sizeof(line), fd)) {
    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = '\0';
    if (!*line || *line == '#') continue;

    char *hash = strchr(line, ':');
    if (!hash) continue;
    *hash++ = '\0';
    if (strcmp(line, user) != 0) continue;

    char *role_s = strchr(hash, ':');
    if (role_s) *role_s++ = '\0';

    if (auth_verify_hash(hash, pass)) role = role_from_string(role_s);
    break;
  }

  fclose(fd);
  return role;
}
