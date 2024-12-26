#include "log.h"
#include "config.h"
#include "db.h"
#include "hmac.h"
#include "base64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libpq-fe.h>
#include <signal.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <sys/time.h>
#include <openssl/crypto.h>

#ifdef __linux__
#include <sys/types.h>
#include <sys/select.h>
#endif

#define ENV_BATCH_TIMEOUT_MS 5000
#define ENV_BATCH_LIMIT 10
#define ENV_DB_CHANNEL_NAME "token_insert"
#define ENV_DB_QUEUE_NAME "user_action_queue"

unsigned char hmac_secret[HMAC_SECRET_SIZE] = {0};
size_t hmac_secretlen = 0;

static volatile sig_atomic_t running = 1;

static void signal_handler(int sig)
{
  log_printf("signal %d received. exiting...", sig);
  running = 0;
}

static bool is_valid_hmac_secrethex(const char *key)
{
  size_t expected_len = HMAC_SECRET_SIZE * 2;

  if (!key || strlen(key) != expected_len)
    return false;

  for (size_t i = 0; i < expected_len; i++)
  {
    if (!isxdigit((unsigned char)key[i]))
      return false;
  }

  return true;
}

static int parse_env_int(const char *env_var, int default_val)
{
  const char *val = getenv(env_var);
  if (!val)
  {
    log_printf("%s not set (default=%d)", env_var, default_val);
    return default_val;
  }

  char *endptr;
  errno = 0;

  long parsed = strtol(val, &endptr, 10);

  if (errno == ERANGE)
  {
    log_printf("value for %s is out of range: %s, using default: %d", env_var, val, default_val);
    return default_val;
  }

  if (endptr == val || *endptr != '\0' || parsed < INT_MIN || parsed > INT_MAX)
  {
    log_printf("invalid value for %s: %s, using default: %d", env_var, val, default_val);
    return default_val;
  }

  return (int)parsed;
}

static size_t hex_to_bytes(unsigned char *b, size_t b_size, const char *hex)
{
  if (!b || !hex)
  {
    log_printf("invalid input");
    return 0;
  }

  size_t hex_len = strlen(hex);
  if (hex_len % 2 != 0)
  {
    log_printf("hex string must have an even length");
    return 0;
  }

  if (b_size < hex_len / 2)
  {
    log_printf("byte array is too small");
    return 0;
  }

  unsigned int byte;
  for (size_t i = 0; i < hex_len; i += 2)
  {
    if (sscanf(hex + i, "%2x", &byte) != 1)
    {
      log_printf("invalid hex character");
      return 0;
    }
    b[i / 2] = (unsigned char)byte;
  }

  return hex_len / 2;
}

static int exit_code(PGconn *conn, int code)
{
  if (conn)
  {
    PQfinish(conn);
  }
  hmac_cleanup();
  return code;
}

static long get_current_time_ms(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
  {
    perror("clock_gettime failed");
    exit(EXIT_FAILURE);
  }
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_microseconds(useconds_t usec)
{
  struct timespec ts;
  ts.tv_sec = usec / 1000000;
  ts.tv_nsec = (usec % 1000000) * 1000;
  nanosleep(&ts, NULL);
}

static int db_dequeue_chunked(PGconn *conn, const char *queue, int remaining, int max_chunk_size)
{
  int result = 0;
  int chunk_size = 0;
  int total = 0;
  while (remaining > 0)
  {
    chunk_size = remaining > max_chunk_size ? max_chunk_size : remaining;
    result = db_dequeue(conn, queue, chunk_size);
    if (result < 0)
    {
      return result;
    }
    total += result;
    remaining -= chunk_size;
    sleep_microseconds(10000); // 10ms
  }
  return total;
}

int main(void)
{
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  const char *conninfo = getenv("DATABASE_URL");
  if (!conninfo)
  {
    log_printf("DATABASE_URL not set");
    return EXIT_FAILURE;
  }

  const char *hmac_secrethex = getenv("SECRET_KEY");
  if (!hmac_secrethex)
  {
    log_printf("SECRET_KEY not set");
    return EXIT_FAILURE;
  }

  if (!is_valid_hmac_secrethex(hmac_secrethex))
  {
    log_printf("SECRET_KEY must be a 64-character hex string");
    return EXIT_FAILURE;
  }

  hmac_secretlen = hex_to_bytes(hmac_secret, sizeof(hmac_secret), hmac_secrethex);
  if (hmac_secretlen == 0)
  {
    log_printf("failed to decode SECRET_KEY");
    return EXIT_FAILURE;
  }

  const char *channel_name = getenv("DB_CHANNEL_NAME");
  if (!channel_name)
  {
    channel_name = ENV_DB_CHANNEL_NAME;
    log_printf("DB_CHANNEL_NAME not set (default=%s)", channel_name);
  }

  const char *queue_name = getenv("DB_QUEUE_NAME");
  if (!queue_name)
  {
    queue_name = ENV_DB_QUEUE_NAME;
    log_printf("DB_QUEUE_NAME not set (default=%s)", queue_name);
  }

  int batch_limit = parse_env_int("BATCH_LIMIT", ENV_BATCH_LIMIT);
  int timeout_ms = parse_env_int("BATCH_TIMEOUT", ENV_BATCH_TIMEOUT_MS);

  log_printf("configured; channel=%s queue=%s limit=%d timeout=%dms", channel_name, queue_name, batch_limit, timeout_ms);

  if (!hmac_init())
  {
    log_printf("failed to init HMAC");
    return EXIT_FAILURE;
  }

  int result;

  PGconn *conn = NULL;

  fd_set active_fds, read_fds;
  int sock;

  struct timeval tv;
  int seen = 0;

  PGnotify *notify = NULL;
  int rc = 0;

  long start = get_current_time_ms();
  long now, elapsed, remaining_ms;

  int ready = -1;

  while (running)
  {
    if (ready < 0)
    {
      if (conn)
      {
        PQfinish(conn);
      }

      if (!db_connect(&conn, conninfo, channel_name))
      {
        log_printf("failed to connect to database: %s", PQerrorMessage(conn));
        return exit_code(conn, EXIT_FAILURE);
      }

      log_printf("connected");

      while (running && (result = db_dequeue_chunked(conn, queue_name, batch_limit, batch_limit)) == batch_limit)
        ;

      if (result < 0)
      {
        return exit_code(conn, EXIT_FAILURE);
      }

      FD_ZERO(&active_fds);
      sock = PQsocket(conn);
      FD_SET(sock, &active_fds);

      seen = 0;
      ready = 0;

      continue;
    }
    else if (ready > 0)
    {
      result = db_dequeue_chunked(conn, queue_name, seen, batch_limit);
      if (result == -2)
      {
        return exit_code(conn, EXIT_FAILURE);
      }
      else if (result == -1)
      {
        log_printf("forcing reconnect...");

        ready = -1; // Force reconnect
        continue;
      }
      else if (result != seen)
      {
        log_printf("WARN: expected %d items to be processed, got %d", seen, result);
      }

      seen = 0;
      ready = 0;
    }

    // Process any pending notifications before select()
    while (running && (notify = PQnotifies(conn)) != NULL)
    {
      PQfreemem(notify);
      if (seen == 0)
      {
        start = get_current_time_ms(); // Received first notification; reset timer
      }
      seen++;
    }

    if (seen >= batch_limit)
    {
      log_printf("max reached; processing %d rows...", seen);

      ready = 1;
      continue; // Skip select() and process immediately
    }

    now = get_current_time_ms();
    elapsed = now - start;
    remaining_ms = timeout_ms - elapsed;

    if (remaining_ms < 0)
    {
      remaining_ms = 0;
    }

    tv.tv_sec = remaining_ms / 1000;
    tv.tv_usec = (remaining_ms % 1000) * 1000;

    read_fds = active_fds;

    rc = select(sock + 1, &read_fds, NULL, NULL, &tv);

    if (rc < 0)
    {
      if (errno == EINTR)
      {
        if (!running)
        {
          break;
        }
        log_printf("select interrupted by signal");
        continue;
      }
      log_printf("select failed: %s (socket=%d)", strerror(errno), sock);
      break;
    }
    else if (rc == 0)
    {                                // Timeout occurred;
      start = get_current_time_ms(); // Reset the timer

      if (seen > 0)
      {
        log_printf("timeout; processing %d rows...", seen);

        ready = 1;
        continue;
      }
      else if ((sock = PQsocket(conn)) < 0)
      {
        log_printf("WARN: socket closed; %s", PQerrorMessage(conn));
        ready = -1;
        break;
      }
    }

    if (!FD_ISSET(sock, &read_fds))
    {
      continue;
    }

    do
    {
      if (!PQconsumeInput(conn))
      {
        log_printf("error consuming input: %s", PQerrorMessage(conn));
        if (PQstatus(conn) != CONNECTION_OK)
        {
          ready = -1;
          break;
        }
      }
    } while (running && PQisBusy(conn));
  }

  return exit_code(conn, EXIT_FAILURE);
}
