/****************************************************************************
 * libs/libbuiltin/libgcc/gcov.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <gcov.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nuttx/lib/lib.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GCOV_DATA_MAGIC       (0x67636461)
#define GCOV_NOTE_MAGIC       (0x67636e6f)
#define GCOV_FILENAME_MAGIC   (0x6763666e)

#define GCOV_TAG_FUNCTION     (0x01000000)
#define GCOV_TAG_COUNTER_BASE (0x01a10000)

#define GCOV_TAG_FOR_COUNTER(count) \
  (GCOV_TAG_COUNTER_BASE + ((uint32_t)(count) << 17))

#ifdef GCOV_12_FORMAT
#  define GCOV_TAG_FUNCTION_LENGTH 12
#else
#  define GCOV_TAG_FUNCTION_LENGTH 3
#endif

#ifdef GCOV_12_FORMAT
#  define GCOV_UNIT_SIZE      4
#else
#  define GCOV_UNIT_SIZE      1
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef unsigned int gcov_unsigned_t;

/* Information about counters for a single function
 *
 * This data is generated by gcc during compilation and doesn't change
 * at run-time.
 */

struct gcov_ctr_info
{
  unsigned int num;      /* Number of counter values for this type */
  FAR gcov_type *values; /* Array of counter values for this type */
};

/* Profiling meta data per function
 *
 * This data is generated by gcc during compilation and doesn't change
 * at run-time.
 */

struct gcov_fn_info
{
  FAR const struct gcov_info *key; /* Comdat key */
  unsigned int ident;              /* Unique ident of function */
  unsigned int lineno_checksum;    /* Function lineno checksum */
  unsigned int cfg_checksum;       /* Function cfg checksum */
  struct gcov_ctr_info ctrs[1];    /* Instrumented counters */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

FAR struct gcov_info *__gcov_info_start;
FAR struct gcov_info *__gcov_info_end;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dump_counter(FAR void *buffer, FAR size_t *off, uint64_t v)
{
  if (buffer)
    {
      *(FAR uint64_t *)((FAR uint8_t *)buffer + *off) = v;
    }

  *off += sizeof(uint64_t);
}

static void dump_unsigned(FAR void *buffer, FAR size_t *off, uint32_t v)
{
  if (buffer)
    {
      *(FAR uint32_t *)((FAR uint8_t *)buffer + *off) = v;
    }

  *off += sizeof(uint32_t);
}

static size_t gcov_convert(FAR uint8_t *buffer,
                           FAR const struct gcov_info *info)
{
  FAR struct gcov_fn_info *gfi;
  FAR struct gcov_ctr_info *gci;
  uint32_t functions;
  uint32_t counts;
  uint32_t value;
  size_t pos = 0;

  /* File header. */

  dump_unsigned(buffer, &pos, GCOV_DATA_MAGIC);
  dump_unsigned(buffer, &pos, info->version);
  dump_unsigned(buffer, &pos, info->stamp);

#ifdef GCOV_12_FORMAT
  dump_unsigned(buffer, &pos, info->checksum);
#endif

  /* Function headers. */

  for (functions = 0; functions < info->n_functions; functions++)
    {
      gfi = info->functions[functions];

      /* Function record. */

      dump_unsigned(buffer, &pos, GCOV_TAG_FUNCTION);
      dump_unsigned(buffer, &pos, GCOV_TAG_FUNCTION_LENGTH);
      dump_unsigned(buffer, &pos, gfi->ident);
      dump_unsigned(buffer, &pos, gfi->lineno_checksum);
      dump_unsigned(buffer, &pos, gfi->cfg_checksum);

      gci = gfi->ctrs;
      for (counts = 0; counts < GCOV_COUNTERS; counts++)
        {
          if (!info->merge[counts])
            {
              continue;
            }

          /* Counter record. */

          dump_unsigned(buffer, &pos, GCOV_TAG_FOR_COUNTER(counts));
          dump_unsigned(buffer, &pos, gci->num * 2 * GCOV_UNIT_SIZE);

          for (value = 0; value < gci->num; value++)
            {
              dump_counter(buffer, &pos, gci->values[value]);
            }

          gci++;
        }
    }

  return pos;
}

static int gcov_process_path(FAR char *prefix, int strip,
                             FAR char *path, FAR char *new_path,
                             size_t len)
{
  FAR char *tokens[64];
  FAR char *filename;
  FAR char *token;
  int token_count = 0;
  int prefix_count;
  int level = 0;
  int ret;
  int i;

  token = strtok(prefix, "/");
  while (token != NULL)
    {
      tokens[token_count++] = token;
      token = strtok(NULL, "/");
    }

  /* Split the path into directories and filename */

  prefix_count = token_count;
  token = strtok(path, "/");
  if (token == NULL)
    {
      return -EINVAL;
    }

  while (token != NULL)
    {
      filename = token;
      if (level++ >= strip)
        {
          /* Skip the specified number of leading directories */

          if (token_count >= sizeof(tokens) / sizeof(tokens[0]))
            {
              return -ENAMETOOLONG;
            }

          tokens[token_count++] = token;
        }

      token = strtok(NULL, "/");
    }

  /* Add the filename */

  if (prefix_count == token_count)
    {
      tokens[token_count++] = filename;
    }

  new_path[0] = '\0';
  tokens[token_count] = NULL;

  /* Check and create directories */

  for (i = 0; i < token_count - 1; i++)
    {
      strcat(new_path, "/");
      strcat(new_path, tokens[i]);
      if (access(new_path, F_OK) != 0)
        {
          ret = mkdir(new_path, 0644);
          if (ret != 0)
            {
              return -errno;
            }
        }
    }

  strcat(new_path, "/");
  strcat(new_path, filename);
  return 0;
}

static int gcov_write_file(FAR const char *filename,
                           FAR const struct gcov_info *info)
{
  FAR uint8_t *buffer;
  size_t written;
  int ret = OK;
  size_t size;
  int fd;

  fd = _NX_OPEN(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      syslog(LOG_ERR, "open %s failed!", filename);
      return -errno;
    }

  size = gcov_convert(NULL, info);
  buffer = lib_malloc(size);
  if (buffer == NULL)
    {
      syslog(LOG_ERR, "gcov alloc failed!");
      _NX_CLOSE(fd);
      return -errno;
    }

  gcov_convert(buffer, info);
  written = _NX_WRITE(fd, buffer, size);
  if (written != size)
    {
      syslog(LOG_ERR, "gcov write file failed!");
      ret = -errno;
    }

  _NX_CLOSE(fd);
  lib_free(buffer);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void __gcov_init(FAR struct gcov_info *info)
{
  info->next = __gcov_info_start;
  __gcov_info_start = info;
}

void __gcov_merge_add(FAR gcov_type *counters, unsigned int n_counters)
{
}

void __gcov_exit(void)
{
}

void __gcov_dump(void)
{
  FAR struct gcov_info *info;
  FAR const char *strip = getenv("GCOV_PREFIX_STRIP");
  FAR const char *prefix = getenv("GCOV_PREFIX");
  FAR char new_path[PATH_MAX];
  FAR char *prefix2;
  int ret;

  if (prefix == NULL)
    {
      syslog(LOG_ERR, "No path prefix specified");
      return;
    }

  prefix2 = strdup(prefix);
  if (prefix2 == NULL)
    {
      syslog(LOG_ERR, "gcov alloc failed!");
      return;
    }

  for (info = __gcov_info_start; info; info = info->next)
    {
      FAR char *filename;

      filename = strdup(info->filename);
      if (filename == NULL)
        {
          syslog(LOG_ERR, "gcov alloc failed! skip %s", info->filename);
          continue;
        }

      /* Process the path, add the prefix and strip the leading directories */

      strcpy(prefix2, prefix);
      ret = gcov_process_path(prefix2, atoi(strip), filename,
                              new_path, PATH_MAX);
      if (ret != 0)
        {
          syslog(LOG_ERR, "gcov process path failed! skip %s",
                 new_path);
          lib_free(filename);
          continue;
        }

      /* Convert the data and write to the file */

      ret = gcov_write_file(new_path, info);
      if (ret != 0)
        {
          syslog(LOG_ERR, "gcov write file failed! skip %s", new_path);
          lib_free(filename);
          continue;
        }

      lib_free(filename);
    }

  lib_free(prefix2);
}

void __gcov_reset(void)
{
  FAR struct gcov_info *info;
  FAR struct gcov_ctr_info *gci;
  uint32_t functions;
  uint32_t counts;

  for (info = __gcov_info_start; info; info = info->next)
    {
      for (functions = 0; functions < info->n_functions; functions++)
        {
          gci = info->functions[functions]->ctrs;

          for (counts = 0; counts < GCOV_COUNTERS; counts++)
            {
              if (!info->merge[counts])
                {
                  continue;
                }

              memset(gci->values, 0, gci->num * sizeof(gcov_type));
              gci++;
            }
        }
    }
}

void __gcov_filename_to_gcfn(FAR const char *filename,
                             FAR void (*dump_fn)(FAR const void *,
                                                 unsigned, FAR void *),
                             FAR void *arg)
{
  if (dump_fn)
    {
      size_t len = strlen(filename);
      dump_fn(filename, len, arg);
    }
}

void __gcov_info_to_gcda(FAR const struct gcov_info *info,
                         FAR void (*filename_fn)(FAR const char *,
                                                 FAR void *),
                         FAR void (*dump_fn)(FAR const void *, size_t,
                                             FAR void *),
                         FAR void *(*allocate_fn)(unsigned int, FAR void *),
                         FAR void *arg)
{
  FAR const char *name = info->filename;
  FAR uint8_t *buffer;
  size_t size;

  filename_fn(name, arg);

  /* Get the size of the buffer */

  size = gcov_convert(NULL, info);

  buffer = lib_malloc(size);
  if (!buffer)
    {
      syslog(LOG_ERR, "gcov alloc failed!");
      return;
    }

  /* Convert the data */

  gcov_convert(buffer, info);
  dump_fn(buffer, size, arg);
  lib_free(buffer);
}
