#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <limits.h>

#include "../../hash.h"
#include "parser.h"
#include "cd_detect.h"
#include "../../compat/fnmatch_rarch.h"
#include "../../file.h"

#include "log.h"

#define SHA1_LEN 40
#define HASH_LEN SHA1_LEN

struct core_file 
{
   char   name[PATH_MAX];
   char   desc[PATH_MAX];
   char serial[PATH_MAX];
   char   size[PATH_MAX];
   char   sha1[PATH_MAX];
};

static int find_hash(int fd, const char *hash, char *game_name, size_t max_len,
      struct core_file *info_content)
{
   if (!info_content)
      return -1;

	while (1)
   {
      if (find_token(fd, "game") < 0) return -1;
      if (find_token(fd, "name") < 0) return -1;

      if (get_token(fd, game_name, max_len) < 0)
         return -1;

      if (find_token(fd, "description") < 0) return -1;

      if (get_token(fd, info_content->desc, sizeof(info_content->desc)) < 0)
         return -1;

      if (find_token(fd, "size") < 0) return -1;

      if (get_token(fd, info_content->size, sizeof(info_content->size)) < 0)
         return -1;

#if 0
      if (find_token(fd, "serial") == 0)
         get_token(fd, info_content->serial, sizeof(info_content->serial));
#endif

      if (find_token(fd, "sha1") < 0) return -1;

      if (get_token(fd, info_content->sha1, MAX_TOKEN_LEN) < 0)
         return -1;

      if (!strcasecmp(hash, info_content->sha1))
         break;
   }
   
   return 0;
}

static int
find_content_canonical_name(const char *hash, char *game_name, size_t max_len,
      struct core_file *info_content)
{
   // TODO: Error handling
   size_t i;
   int rv, fd, offs;
   const char *dat_name, *dat_name_dot;
   struct string_list *files = dir_list_new("db", "dat", false);

   if (!files)
      return -1;

   for (i = 0; i < files->size; i++)
   {
      dat_name = path_basename(files->elems[i].data);

      dat_name_dot = strchr(dat_name, '.');
      if (!dat_name_dot)
         continue;

      offs = dat_name_dot - dat_name + 1;
      memcpy(game_name, dat_name, offs);

      fd = open(files->elems[i].data, O_RDONLY);
      if (fd < 0)
         continue;

      if (find_hash(fd, hash,game_name + offs,
               max_len - offs, info_content) == 0)
      {
         rv = 0;
         close(fd);
         goto clean;
      }

      close(fd);
   }
   rv = -1;
clean:
   dir_list_free(files);
   return rv;
}

static int get_sha1(const char *path, char *result)
{
   int rv;
   unsigned char buff[4096];
   SHA1Context sha;
   int fd = open(path, O_RDONLY);

   if (fd < 0)
      return -errno;

   SHA1Reset(&sha);
   rv = 1;
   while (rv > 0)
   {
      rv = read(fd, buff, 4096);
      if (rv < 0)
      {
         close(fd);
         return -errno;
      }

      SHA1Input(&sha, buff, rv);
   }

   if (!SHA1Result(&sha))
   {
      close(fd);
      return -1;
   }

   sprintf(result, "%08X%08X%08X%08X%08X",
         sha.Message_Digest[0],
         sha.Message_Digest[1],
         sha.Message_Digest[2],
         sha.Message_Digest[3], sha.Message_Digest[4]);
   close(fd);
   return 0;
}

struct RunInfo
{
   char broken_cores[PATH_MAX];
   int multitap;
   int dualanalog;
   char system[10];
};

static int read_launch_conf(struct RunInfo *info, const char *game_name)
{
	int fd = open("./launch.conf", O_RDONLY);
	int rv;
	int bci = 0;
	char token[MAX_TOKEN_LEN];
	if (fd < 0)
		return -errno;

	while (1)
   {
		if ((rv = get_token(fd, token, MAX_TOKEN_LEN)) < 0)
			goto clean;

		if (rl_fnmatch(token, game_name, 0) != 0)
      {
			if ((rv = find_token(fd, ";")) < 0)
				goto clean;
			continue;
		}

		LOG_DEBUG("Matched rule '%s'", token);
		break;
	}

	if ((rv = get_token(fd, token, MAX_TOKEN_LEN)) < 0)
		goto clean;

	while (strcmp(token, ";") != 0)
   {
      if (!strcmp(token, "multitap"))
         info->multitap = 1;
      else if (!strcmp(token, "dualanalog"))
         info->dualanalog = 1;
      else if (token[0] == '!')
      {
         strncpy(&info->broken_cores[bci], &token[1], PATH_MAX - bci);
         bci += strnlen(&token[1], PATH_MAX) + 1;
      }

      if ((rv = get_token(fd, token, MAX_TOKEN_LEN)) < 0)
         goto clean;
   }
	rv = 0;
 clean:
	close(fd);
	return rv;
}

static int get_run_info(struct RunInfo *info, const char *game_name)
{
   int i;

   memset(info, 0, sizeof(struct RunInfo));

   for (i = 0; i < 9; i++)
   {
      if (game_name[i] == '.')
         break;
      info->system[i] = game_name[i];
   }
   info->system[i] = '\0';
   info->multitap = 0;
   info->dualanalog = 0;

   read_launch_conf(info, game_name);
   return 0;
}


static int detect_content_game(const char *path, char *game_name,
      size_t max_len, struct core_file *info_content)
{
	char hash[HASH_LEN + 1], *substr;
	const char *suffix = strrchr(path, '.');

	if (!suffix)
   {
		LOG_WARN("Could not find extension for: %s", path);
		return -EINVAL;
	}

	memset(hash, 0, sizeof(hash));

   get_sha1(path, hash);
#if 0
	if (rv < 0)
		LOG_WARN("Could not calculate hash: %s", strerror(-rv));
#endif

	if (find_content_canonical_name(hash, game_name, max_len, info_content) < 0)
   {
		LOG_DEBUG("Could not detect content with hash `%s`.", hash);
		return -EINVAL;
	}

   substr = strrchr(game_name, '.');
   if (substr)
      strcpy(info_content->name, substr + 1);

	return 0;
}

int detect_file(const char *path, char *game_name, size_t max_len,
      struct core_file *info_content)
{
   if ((!strcasecmp(path + strlen(path) - 4, ".cue")) ||
         (!strcasecmp(path + strlen(path) - 4, ".m3u")))
   {
      LOG_INFO("Starting CD game content detection...");
      return detect_cd_game(path, game_name, max_len);
   }

   return detect_content_game(path, game_name, max_len, info_content);
}

#ifndef RARCH_CONSOLE
#ifndef __APPLE__
#undef main
#endif
int main(int argc, char *argv[])
{
   struct RunInfo info;
   struct core_file info_content;
   int rv;
   char game_name[MAX_TOKEN_LEN];
   char *path = argv[1];

   if (argc < 2)
   {
      printf("usage: retrolaunch <ROM>\n");
      return -1;
   }

   LOG_INFO("Analyzing '%s'", path);
   if ((rv = detect_file(path, game_name, MAX_TOKEN_LEN, &info_content)) < 0)
   {
      LOG_WARN("Could not detect game: %s", strerror(-rv));
      return -1;
   }

   LOG_INFO("Content Name        : `%s`", info_content.name);
   LOG_INFO("Content Description : `%s`", info_content.desc);
   LOG_INFO("Content Serial      : `%s`", info_content.serial);
   LOG_INFO("Content Size        : `%s`", info_content.size);
   LOG_INFO("Content SHA-1       : `%s`", info_content.sha1);

   if ((rv = get_run_info(&info, game_name)) < 0)
   {
      LOG_WARN("Could not detect run info: %s", strerror(-rv));
      return -1;
   }

   return 0;
}
#endif
