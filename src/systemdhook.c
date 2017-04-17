#define _GNU_SOURCE
#include <stdio.h>
#include <libgen.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mount.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/limits.h>
#include <selinux/selinux.h>
#include <yajl/yajl_tree.h>

#include "config.h"

#include <libmount/libmount.h>

static unsigned long get_mem_total() {
	struct sysinfo info;
	int ret = sysinfo(&info);
	if (ret < 0) {
		return ret;
	}
	return info.totalram;
}

#define _cleanup_(x) __attribute__((cleanup(x)))

static inline void freep(void *p) {
	free(*(void**) p);
}

static inline void closep(int *fd) {
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
}

static inline void fclosep(FILE **fp) {
	if (*fp)
		fclose(*fp);
	*fp = NULL;
}

static inline void mnt_free_iterp(struct libmnt_iter **itr) {
	if (*itr)
		mnt_free_iter(*itr);
	*itr=NULL;
}

static inline void mnt_free_fsp(struct libmnt_fs **itr) {
	if (*itr)
		mnt_free_fs(*itr);
	*itr=NULL;
}

#define _cleanup_free_ _cleanup_(freep)
#define _cleanup_close_ _cleanup_(closep)
#define _cleanup_fclose_ _cleanup_(fclosep)
#define _cleanup_mnt_iter_ _cleanup_(mnt_free_iterp)
#define _cleanup_mnt_fs_ _cleanup_(mnt_free_fsp)

#define DEFINE_CLEANUP_FUNC(type, func)                         \
	static inline void func##p(type *p) {                   \
		if (*p)                                         \
			func(*p);                               \
	}                                                       \

DEFINE_CLEANUP_FUNC(yajl_val, yajl_tree_free)

#define pr_perror(fmt, ...) syslog(LOG_ERR, "systemdhook <error>: " fmt ": %m\n", ##__VA_ARGS__)
#define pr_pinfo(fmt, ...) syslog(LOG_INFO, "systemdhook <info>: " fmt "\n", ##__VA_ARGS__)
#define pr_pdebug(fmt, ...) syslog(LOG_DEBUG, "systemdhook <debug>: " fmt "\n", ##__VA_ARGS__)

#define BUFLEN 1024
#define CONFIGSZ 65536

#define CGROUP_ROOT "/sys/fs/cgroup"
#define CGROUP_SYSTEMD CGROUP_ROOT"/systemd"

static int makepath(char *dir, mode_t mode)
{
    if (!dir) {
	errno = EINVAL;
	return -1;
    }

    if (strlen(dir) == 1 && dir[0] == '/')
	return 0;

    makepath(dirname(strdupa(dir)), mode);

    return mkdir(dir, mode);
}

static int bind_mount(const char *src, const char *dest, int readonly) {
	if (mount(src, dest, "bind", MS_BIND, NULL) == -1) {
		pr_perror("Failed to mount %s on %s", src, dest);
		return -1;
	}
	//  Remount bind mount to read/only if requested by the caller
	if (readonly) {
		if (mount(src, dest, "bind", MS_REMOUNT|MS_BIND|MS_RDONLY, "") == -1) {
			pr_perror("Failed to remount %s readonly", dest);
			return -1;
		}
	}
	return 0;
}

static int chcon(const char *path, const char *label) {
	DIR *dir;
	struct dirent *ent;
	if ((dir = opendir (path)) != NULL) {
		/* print all the files and directories within directory */
		while ((ent = readdir (dir)) != NULL) {
			_cleanup_free_ char *full_path = NULL;
			if (asprintf(&full_path, "%s/%s", path, ent->d_name) < 0) {
				pr_perror("Failed to create path for chcon");
				closedir(dir);
				return -1;

			}
			if (setfilecon (full_path, label) < 0) {
				pr_perror("Failed to set context %s on %s", label, path);
			}
		}
		closedir (dir);
	} else {
		/* could not open directory */
		pr_perror("Failed to set labels on %s", path);
		return -1;
	}
	return 0;
}

/*
 * Get the contents of the file specified by its path
 */
static char *get_file_contents(const char *path) {
	_cleanup_close_ int fd = -1;
	if ((fd = open(path, O_RDONLY)) == -1) {
		pr_perror("Failed to open file for reading");
		return NULL;
	}

	char buffer[256];
	ssize_t rd;
	rd = read(fd, buffer, 256);
	if (rd == -1) {
		pr_perror("Failed to read file contents");
		return NULL;
	}

	buffer[rd] = '\0';

	return strdup(buffer);
}

/*
 * Get the cgroup file system path for the specified process id
 */
static char *get_process_cgroup_subsystem_path(int pid, const char *subsystem) {
	_cleanup_free_ char *cgroups_file_path = NULL;
	int rc;
	rc = asprintf(&cgroups_file_path, "/proc/%d/cgroup", pid);
	if (rc < 0) {
		pr_perror("Failed to allocate memory for cgroups file path");
		return NULL;
	}

	_cleanup_fclose_ FILE *fp = NULL;
	fp = fopen(cgroups_file_path, "r");
	if (fp == NULL) {
		pr_perror("Failed to open cgroups file");
		return NULL;
	}

	_cleanup_free_ char *line = NULL;
	ssize_t read;
	size_t len = 0;
	char *ptr;
	char *subsystem_path = NULL;
	while ((read = getline(&line, &len, fp)) != -1) {
		pr_pdebug("%s", line);
		ptr = strchr(line, ':');
		if (ptr == NULL) {
			pr_perror("Error parsing cgroup, ':' not found: %s", line);
			return NULL;
		}
		pr_pdebug("%s", ptr);
		ptr++;
		if (!strncmp(ptr, subsystem, strlen(subsystem))) {
			pr_pdebug("Found");
			char *path = strchr(ptr, '/');
			if (path == NULL) {
				pr_perror("Error finding path in cgroup: %s", line);
				return NULL;
			}
			pr_pdebug("PATH: %s", path);
			const char *subpath = strchr(subsystem, '=');
			if (subpath == NULL) {
				subpath = subsystem;
			} else {
				subpath++;
			}

			rc = asprintf(&subsystem_path, "%s/%s%s", CGROUP_ROOT, subpath, path);
			if (rc < 0) {
				pr_perror("Failed to allocate memory for subsystemd path");
				return NULL;
			}
			pr_pdebug("SUBSYSTEM_PATH: %s", subsystem_path);
			subsystem_path[strlen(subsystem_path) - 1] = '\0';
			return subsystem_path;
		}
	}

	return NULL;
}

/*
   Mount a tmpfs on the /sys/fs/systemd directory inside of container.
   Create a systemd subdir
   Remount the tmpfs read/only
 */
static int mount_cgroup(const char *rootfs, const char *options, char *systemd_path)
{
	_cleanup_free_ char *cgroup_path = NULL;

	if (asprintf(&cgroup_path, "%s/%s", rootfs, CGROUP_ROOT) < 0) {
		pr_perror("Failed to create path for %s", CGROUP_ROOT);
		return -1;
	}
	if ((makepath(cgroup_path, 0755) == -1) && (errno != EEXIST)) {
		pr_perror("Failed to mkdir new dest: %s", cgroup_path);
		return -1;
	}
	/* Mount tmpfs at new cgroup directory */
	if (mount("tmpfs", cgroup_path, "tmpfs", MS_NODEV|MS_NOSUID, options) == -1) {
		pr_perror("Failed to mount tmpfs at %s", cgroup_path);
		return -1;
	}
	if ((makepath(systemd_path, 0755) == -1) && (errno != EEXIST)) {
		pr_perror("Failed to mkdir new dest: %s", systemd_path);
		return -1;
	}
	if (mount(cgroup_path, cgroup_path, "bind", MS_REMOUNT|MS_BIND|MS_RDONLY, "") == -1) {
		pr_perror("Failed to remount %s readonly", cgroup_path);
		return -1;
	}
	return 0;
}

static bool contains_mount(const char **config_mounts, unsigned len, const char *mount) {
	for (unsigned i = 0; i < len; i++) {
		if (!strcmp(mount, config_mounts[i])) {
			pr_pdebug("%s already present as a mount point in container configuration, skipping\n", mount);
			return true;
		}
	}
	return false;
}

/*
 * Move specified mount to temporary directory
 */
static int move_mount_to_tmp(const char *rootfs, const char *tmp_dir, const char *mount_dir, int offset)
{
	int rc;
	_cleanup_free_ char *src = NULL;
	_cleanup_free_ char *dest = NULL;
	_cleanup_free_ char *post = NULL;

	rc = asprintf(&src, "%s/%s", rootfs, mount_dir);
	if (rc < 0) {
		pr_perror("Failed to allocate memory for src");
		return -1;
	}

	/* Find the second '/' to get the postfix */
	post = strdup(&mount_dir[offset]);

	if (!post) {
		pr_perror("Failed to allocate memory for postfix");
		return -1;
	}

	rc = asprintf(&dest, "%s/%s", tmp_dir, post);
	if (rc < 0) {
		pr_perror("Failed to allocate memory for dest");
		return -1;
	}

	if (makepath(dest, 0755) == -1) {
		if (errno != EEXIST) {
			pr_perror("Failed to mkdir new dest: %s", dest);
			return -1;
		}
	}

	/* Move the mount to temporary directory */
	if ((mount(src, dest, "", MS_MOVE, "") == -1)) {
		pr_perror("Failed to move mount %s to %s", src, dest);
		return -1;
	}

	return 0;
}

static int move_mounts(const char *rootfs,
		       const char *path,
		       const char **config_mounts,
		       unsigned config_mounts_len,
		       char *options
	) {

	char mount_dir[PATH_MAX];
	snprintf(mount_dir, PATH_MAX, "%s%s", rootfs, path);

	/* Create a temporary directory to move the PATH mounts to */
	char temp_template[] = "/tmp/ocitmp.XXXXXX";

	char *tmp_dir = mkdtemp(temp_template);
	if (tmp_dir == NULL) {
		pr_perror("Failed to create temporary directory for mounts");
		return -1;
	}

	/* Create the PATH directory */
	if (!contains_mount(config_mounts, config_mounts_len, path)) {
		if (mkdir(mount_dir, 0755) == -1) {
			if (errno != EEXIST) {
				pr_perror("Failed to mkdir: %s", mount_dir);
				return -1;
			}
		}

		/* Mount tmpfs at new temp directory */
		if (mount("tmpfs", tmp_dir, "tmpfs", MS_NODEV|MS_NOSUID, options) == -1) {
			pr_perror("Failed to mount tmpfs at %s", tmp_dir);
			return -1;
		}

		/* Move other user specified mounts under PATH to temporary directory */
		for (unsigned i = 0; i < config_mounts_len; i++) {
			/* Match destinations that begin with PATH */
			if (!strncmp(path, config_mounts[i], strlen(path))) {
				if (move_mount_to_tmp(rootfs, tmp_dir, config_mounts[i], strlen(path)) < 0) {
					pr_perror("Failed to move %s to %s", config_mounts[i], tmp_dir);
					return -1;
				}
			}
		}

		/* Move temporary directory to PATH */
		if ((mount(tmp_dir, mount_dir, "", MS_MOVE, "") == -1)) {
			pr_perror("Failed to move mount %s to %s", tmp_dir, mount_dir);
			return -1;
		}
	}

	/* Remove the temp directory for PATH */
	if (rmdir(tmp_dir) < 0) {
		pr_perror("Failed to remove %s", tmp_dir);
		return -1;
	}
	return 0;
}

static int prestart(const char *rootfs,
		const char *id,
		int pid,
		const char *mount_label,
		const char **config_mounts,
		unsigned config_mounts_len)
{
	_cleanup_close_  int fd = -1;
	_cleanup_free_   char *options = NULL;

	int rc = -1;
	char process_mnt_ns_fd[PATH_MAX];
	snprintf(process_mnt_ns_fd, PATH_MAX, "/proc/%d/ns/mnt", pid);

	fd = open(process_mnt_ns_fd, O_RDONLY);
	if (fd < 0) {
		pr_perror("Failed to open mnt namespace fd %s", process_mnt_ns_fd);
		return -1;
	}

	/* Join the mount namespace of the target process */
	if (setns(fd, 0) == -1) {
		pr_perror("Failed to setns to %s", process_mnt_ns_fd);
		return -1;
	}
	close(fd);
	fd = -1;

	/* Switch to the root directory */
	if (chdir("/") == -1) {
		pr_perror("Failed to chdir");
		return -1;
	}

	if (!strcmp("", mount_label)) {
		rc = asprintf(&options, "mode=755,size=65536k");
	} else {
		rc = asprintf(&options, "mode=755,size=65536k,context=\"%s\"", mount_label);
	}
	if (rc < 0) {
		pr_perror("Failed to allocate memory for context");
		return -1;
	}

	rc = move_mounts(rootfs, "/run", config_mounts, config_mounts_len, options);
	if (rc < 0) {
		return rc;
	}

	_cleanup_free_ char *memory_cgroup_path = NULL;
	memory_cgroup_path = get_process_cgroup_subsystem_path(pid, "memory");
	if (!memory_cgroup_path) {
		pr_perror("Failed to get memory subsystem path for the process");
		return -1;
	}

	char memory_limit_path[PATH_MAX];
	snprintf(memory_limit_path, PATH_MAX, "%s/memory.limit_in_bytes", memory_cgroup_path);

	pr_pdebug("memory path: %s", memory_limit_path);

	_cleanup_free_ char *memory_limit_str = NULL;
	memory_limit_str = get_file_contents(memory_limit_path);
	if (!memory_limit_str) {
		pr_perror("Failed to get memory limit from cgroups");
		return -1;
	}

	pr_pdebug("LIMIT: %s\n", memory_limit_str);

	char memory_str[PATH_MAX];
	uint64_t total_memory = 0;
	uint64_t memory_limit_in_bytes = 0;
	char *ptr = NULL;

	memory_limit_in_bytes = strtoull(memory_limit_str, &ptr, 10);

	pr_pdebug("Limit in bytes: ""%" PRIu64 "\n", memory_limit_in_bytes);

	total_memory = get_mem_total();
	if (memory_limit_in_bytes < total_memory) {
		/* Set it to half of limit in kb */
		uint64_t memory_limit_in_kb = memory_limit_in_bytes / 2048;
		snprintf(memory_str, sizeof(memory_str)-1 , ",size=%" PRIu64 "k", memory_limit_in_kb);
	} else {
		strcpy(memory_str, "");
	}

	char tmp_dir[PATH_MAX];
	snprintf(tmp_dir, PATH_MAX, "%s/tmp", rootfs);

	/*
	   Create a /var/log/journal directory on the host and mount it into
	   the container.
	*/
	if (!contains_mount(config_mounts, config_mounts_len, "/var/log/journal")) {
		char journal_dir[PATH_MAX];
		snprintf(journal_dir, PATH_MAX, "/var/log/journal/%.32s", id);
		char cont_journal_dir[PATH_MAX];
		snprintf(cont_journal_dir, PATH_MAX, "%s/var/log/journal", rootfs);
		if (makepath(journal_dir, 0755) == -1) {
			if (errno != EEXIST) {
				pr_perror("Failed to mkdir journal dir: %s", journal_dir);
				return -1;
			}
		}

		if (strcmp("", mount_label)) {
			rc = setfilecon(journal_dir, (security_context_t)mount_label);
			if (rc < 0) {
				pr_perror("Failed to set journal dir selinux context");
				return -1;
			}
		}

		/* Attempt to creare /var/log/journal inside of rootfs,
		   if successful, or directory exists, mount tmpfs on top of
		   it, so that systemd can write journal to it, even in
		   read/only images
		*/
		if ((makepath(cont_journal_dir, 0755) == 0) ||
		    (errno == EEXIST)) {
			snprintf(cont_journal_dir, PATH_MAX, "%s%s", rootfs, journal_dir);
			/* Mount tmpfs at /var/log/journal for systemd */
			rc = move_mounts(rootfs, "/var/log/journal", config_mounts, config_mounts_len, options);
			if (rc < 0) {
				return rc;
			}
		} else {
			/* If you can't create /var/log/journal inside of rootfs,
			   crate /run/journal instead, systemd should write here
			   if it is not allowed to write to /var/log/journal
			*/
			snprintf(cont_journal_dir, PATH_MAX, "%s/run/journal/%.32s", rootfs, id);
		}
		if ((makepath(cont_journal_dir, 0755) == -1) &&
		    (errno != EEXIST)) {
			pr_perror("Failed to mkdir container journal dir: %s", cont_journal_dir);
			return -1;
		}

		/* Mount journal directory at cont_journal_dir path in the container */
		if (bind_mount(journal_dir, cont_journal_dir, false) == -1) {
			return -1;
		}
	}

	/* Create the /tmp directory */
	if (!contains_mount(config_mounts, config_mounts_len, "/tmp")) {
		if (mkdir(tmp_dir, 0755) == -1) {
			if (errno != EEXIST) {
				pr_perror("Failed to mkdir: %s", tmp_dir);
				return -1;
			}
		}

		free(options); options=NULL;
		if (!strcmp("", mount_label)) {
			rc = asprintf(&options, "mode=1777%s", memory_str);
		} else {
			rc = asprintf(&options, "mode=1777%s,context=\"%s\"", memory_str, mount_label);
		}
		if (rc < 0) {
			pr_perror("Failed to allocate memory for context");
			return -1;
		}

		/* Mount tmpfs at /tmp for systemd */
		rc = move_mounts(rootfs, "/tmp", config_mounts, config_mounts_len, options);
		if (rc < 0) {
			return rc;
		}
	}

	/*
	 * initialize libmount
	 */

	/*
	   if CGROUP_ROOT is not bind mounted, we need to create a tmpfs on
	   it, and then create the systemd directory underneath it
	*/

	_cleanup_free_ char *systemd_path = NULL;
	if (asprintf(&systemd_path, "%s/%s", rootfs, CGROUP_SYSTEMD) < 0) {
		pr_perror("Failed to create path for %s", CGROUP_ROOT);
		return -1;
	}
	if (!contains_mount(config_mounts, config_mounts_len, CGROUP_ROOT)) {
		rc = mount_cgroup(rootfs, options, systemd_path);
	} else {
		if ((makepath(systemd_path, 0755) == -1) && (errno != EEXIST)) {
			pr_perror("Failed to mkdir new dest: %s", systemd_path);
			return -1;
		}
	}

	if (bind_mount(CGROUP_SYSTEMD, systemd_path, true)) {
		pr_perror("Failed to bind mount %s on %s", CGROUP_SYSTEMD, systemd_path);
		return -1;
	}

	/*
	   Mount the writable systemd hierarchy into the container
	*/
	_cleanup_free_ char *named_path = NULL;
	named_path = get_process_cgroup_subsystem_path(pid, "name=systemd");
	_cleanup_free_ char *systemd_named_path = NULL;
	if (asprintf(&systemd_named_path, "%s/%s", rootfs, named_path) < 0) {
		pr_perror("Failed to create path for %s/%s", rootfs, systemd_named_path);
		return -1;
	}
	if (bind_mount(named_path, systemd_named_path, false)) {
		pr_perror("Failed to bind mount %s on %s", CGROUP_SYSTEMD, systemd_named_path);
		return -1;
	}
	if (chcon(systemd_named_path, mount_label) < 0) {
		return -1;
	}

	/*
	   Create /etc/machine-id if it does not exist
	*/
	if (!contains_mount(config_mounts, config_mounts_len, "/etc/machine-id")) {
		char mid_path[PATH_MAX];
		snprintf(mid_path, PATH_MAX, "%s/etc/machine-id", rootfs);
		fd = open(mid_path, O_CREAT|O_WRONLY, 0444);
		if (fd < 0) {
			pr_perror("Failed to open %s for writing", mid_path);
			return -1;
		}

		rc = dprintf(fd, "%.32s\n", id);
		if (rc < 0) {
			pr_perror("Failed to write id to %s", mid_path);
			return -1;
		}
	}

	return 0;
}

static int poststop(const char *rootfs,
		const char **config_mounts,
		unsigned config_mounts_len)
{
	if (contains_mount(config_mounts, config_mounts_len, "/etc/machine-id")) {
		return 0;
	}

	int ret = 0;
	char mid_path[PATH_MAX];
	snprintf(mid_path, PATH_MAX, "%s/etc/machine-id", rootfs);

	if (unlink(mid_path) != 0 && (errno != ENOENT)) {
		pr_perror("Unable to remove %s", mid_path);
		ret = 1;
	}

	return ret;
}

int main(int argc, char *argv[])
{
	size_t rd;
	_cleanup_(yajl_tree_freep) yajl_val node = NULL;
	_cleanup_(yajl_tree_freep) yajl_val config_node = NULL;
	char errbuf[BUFLEN];
	char stateData[CONFIGSZ];
	char config_file_name[PATH_MAX];
	char *configData;
	off_t configData_size;
	struct stat configData_stat;
	_cleanup_fclose_ FILE *fp = NULL;

	memset(stateData, 0, CONFIGSZ);
	memset(errbuf, 0, BUFLEN);

	/* Read the entire config file from stdin */
	rd = fread((void *)stateData, 1, sizeof(stateData) - 1, stdin);
	if (rd == 0 && !feof(stdin)) {
		pr_perror("Error encountered on file read");
		return EXIT_FAILURE;
	} else if (rd >= sizeof(stateData) - 1) {
		pr_perror("Config file too big");
		return EXIT_FAILURE;
	}

	/* Parse the state */
	node = yajl_tree_parse((const char *)stateData, errbuf, sizeof(errbuf));
	if (node == NULL) {
		pr_perror("parse_error: ");
		if (strlen(errbuf)) {
			pr_perror(" %s", errbuf);
		} else {
			pr_perror("unknown error");
		}
		return EXIT_FAILURE;
	}

	/* Extract values from the state json */
	const char *root_path[] = { "root", (const char *)0 };
	yajl_val v_root = yajl_tree_get(node, root_path, yajl_t_string);
	if (!v_root) {
		pr_perror("root not found in state");
		return EXIT_FAILURE;
	}
	char *rootfs = YAJL_GET_STRING(v_root);

	const char *pid_path[] = { "pid", (const char *) 0 };
	yajl_val v_pid = yajl_tree_get(node, pid_path, yajl_t_number);
	if (!v_pid) {
		pr_perror("pid not found in state");
		return EXIT_FAILURE;
	}
	int target_pid = YAJL_GET_INTEGER(v_pid);

	const char *id_path[] = { "id", (const char *)0 };
	yajl_val v_id = yajl_tree_get(node, id_path, yajl_t_string);
	if (!v_id) {
		pr_perror("id not found in state");
		return EXIT_FAILURE;
	}
	char *id = YAJL_GET_STRING(v_id);

	/* bundle_path must be specified for the OCI hooks, and from there we read the configuration file.
	   If it is not specified, then check that it is specified on the command line.  */
	const char *bundle_path[] = { "bundlePath", (const char *)0 };
	yajl_val v_bundle_path = yajl_tree_get(node, bundle_path, yajl_t_string);
	if (v_bundle_path) {
		snprintf(config_file_name, PATH_MAX, "%s/config.json", YAJL_GET_STRING(v_bundle_path));
		fp = fopen(config_file_name, "r");
	} else {
		char msg[] = "bundlePath not found in state";
		snprintf(config_file_name, PATH_MAX, "%s", msg);
	}

	/* Parse the config file */

	if (fp == NULL) {
		pr_perror("Failed to open config file: %s", config_file_name);
		return EXIT_FAILURE;
	}

	if (fstat(fileno(fp), &configData_stat) == -1) {
		pr_perror("Failed to determine size of config file: %s", config_file_name);
		return EXIT_FAILURE;
	}
	if (configData_stat.st_size == 0) {
		pr_perror("config file %s is empty", config_file_name);
		return EXIT_FAILURE;
	}
	configData_size = configData_stat.st_size;

	configData = (char *)malloc((size_t)configData_size+1);
	if (configData == NULL) {
		pr_perror("Failed to allocate buffer for config data: size=%ld", configData_size+1);
		return EXIT_FAILURE;
	}

	rd = fread((void *)configData, 1, (size_t)configData_size, fp);
	if (rd != (size_t)configData_size) {
		pr_perror("error encountered on config file read");
		return EXIT_FAILURE;
	}

	pr_pdebug("parse %ld bytes of config data from file %s", configData_size, config_file_name);

	configData[configData_size] = 0; /* make sure the JSON string is NULL-terminated */
	config_node = yajl_tree_parse((const char *)configData, errbuf, sizeof(errbuf));
	if (config_node == NULL) {
		pr_perror("parse_error: ");
		if (strlen(errbuf)) {
			pr_perror(" %s", errbuf);
		} else {
			pr_perror("unknown error");
		}
		return EXIT_FAILURE;
	}

	char *mount_label = NULL;
	const char **config_mounts = NULL;
	unsigned config_mounts_len = 0;

	/* Extract values from the config json */
	const char *mount_label_path[] = { "linux", "mountLabel", (const char *)0 };
	yajl_val v_mount = yajl_tree_get(config_node, mount_label_path, yajl_t_string);
	mount_label = v_mount ? YAJL_GET_STRING(v_mount) : "";

	const char *mount_points_path[] = {"mounts", (const char *)0 };
	yajl_val v_mounts = yajl_tree_get(config_node, mount_points_path, yajl_t_array);
	if (!v_mounts) {
		pr_perror("mounts not found in config");
		return EXIT_FAILURE;
	}

	config_mounts_len = YAJL_GET_ARRAY(v_mounts)->len;
	config_mounts = malloc (sizeof(char *) * (config_mounts_len + 1));
	if (! config_mounts) {
		pr_perror("error malloc'ing");
		return EXIT_FAILURE;
	}

	for (unsigned int i = 0; i < config_mounts_len; i++) {
		yajl_val v_mounts_values = YAJL_GET_ARRAY(v_mounts)->values[i];

		const char *destination_path[] = {"destination", (const char *)0 };
		yajl_val v_destination = yajl_tree_get(v_mounts_values, destination_path, yajl_t_string);
		if (!v_destination) {
			pr_perror("Cannot find mount destination");
			return EXIT_FAILURE;
		}
		config_mounts[i] = YAJL_GET_STRING(v_destination);
	}

	const char *args_path[] = {"process", "args", (const char *)0 };
	yajl_val v_args = yajl_tree_get(config_node, args_path, yajl_t_array);
	if (!v_args) {
		pr_perror("args not found in config");
		return EXIT_FAILURE;
	}

	const char *envs[] = {"process", "env", (const char *)0 };
	yajl_val v_envs = yajl_tree_get(config_node, envs, yajl_t_array);
	if (v_envs) {
		for (unsigned int i = 0; i < YAJL_GET_ARRAY(v_envs)->len; i++) {
			yajl_val v_env = YAJL_GET_ARRAY(v_envs)->values[i];
			char *str = YAJL_GET_STRING(v_env);
			if (strncmp (str, "container_uuid=", strlen ("container_uuid=")) == 0) {
				id = strdup (str + strlen ("container_uuid="));
				/* systemd expects $container_uuid= to be an UUID but then treat it as
				   not containing any '-'.  Do the same here.  */
				char *to = id;
				for (char *from = to; *from; from++) {
					if (*from != '-')
						*to++ = *from;
				}
				*to = '\0';
			}
		}
	}

#if ARGS_CHECK
	char *cmd = NULL;
	yajl_val v_arg0_value = YAJL_GET_ARRAY(v_args)->values[0];
	cmd = YAJL_GET_STRING(v_arg0_value);
	/* Don't do anything if init is actually container runtime bind mounted /dev/init */
	if (!strcmp(cmd, "/dev/init")) {
		pr_pdebug("Skipping as container command is /dev/init, not systemd init\n");
		return EXIT_SUCCESS;
	}
	char *cmd_file_name = basename(cmd);
	if (strcmp("init", cmd_file_name) && strcmp("systemd", cmd_file_name)) {
		pr_pdebug("Skipping as container command is %s, not init or systemd\n", cmd);
		return EXIT_SUCCESS;
	}
#endif

	/* OCI hooks set target_pid to 0 on poststop, as the container process already
	   exited.  If target_pid is bigger than 0 then it is the prestart hook.  */
	if ((argc > 2 && !strcmp("prestart", argv[1])) || target_pid) {
		if (prestart(rootfs, id, target_pid, mount_label, config_mounts, config_mounts_len) != 0) {
			return EXIT_FAILURE;
		}
	} else if ((argc > 2 && !strcmp("poststop", argv[1])) || (target_pid == 0)) {
		if (poststop(rootfs, config_mounts, config_mounts_len) != 0) {
			return EXIT_FAILURE;
		}
	} else {
		pr_perror("command not recognized: %s", argv[1]);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
