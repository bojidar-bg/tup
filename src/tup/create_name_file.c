/* vim: set ts=8 sw=8 sts=8 noet tw=78:
 *
 * tup - A file-based build system
 *
 * Copyright (C) 2008-2024  Mike Shal <marfey@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* _ATFILE_SOURCE for readlinkat */
#define _ATFILE_SOURCE
#include "fileio.h"
#include "db.h"
#include "compat.h"
#include "pel_group.h"
#include "entry.h"
#include "option.h"
#include "variant.h"
#include "config.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int ghost_to_file(struct tup_entry *tent);

static void (*rmdir_callback)(tupid_t tupid);

int create_name_file(tupid_t dt, const char *file, struct timespec mtime,
		     struct tup_entry **entry)
{
	struct tup_entry *dtent;
	if(tup_entry_add(dt, &dtent) < 0)
		return -1;
	if(tup_db_node_insert_tent(dtent, file, -1, TUP_NODE_FILE, mtime, -1, entry) < 0)
		return -1;
	if(tup_db_add_create_list(dt) < 0)
		return -1;
	if(make_dirs_normal(dtent) < 0)
		return -1;
	return 0;
}

tupid_t create_command_file(tupid_t dt, const char *cmd, const char *display, int displaylen, const char *flags, int flagslen)
{
	struct tup_entry *tent;
	struct tup_entry *dtent;
	if(tup_entry_add(dt, &dtent) < 0)
		return -1;
	tent = tup_db_create_node_part_display(dtent, cmd, -1, display, displaylen, flags, flagslen,
					       TUP_NODE_CMD, -1, NULL);
	if(tent)
		return tent->tnode.tupid;
	return -1;
}

int make_dirs_normal(struct tup_entry *dtent)
{
	while(dtent && dtent->type == TUP_NODE_GENERATED_DIR) {
		printf("tup: Converting ");
		print_tup_entry(stdout, dtent);
		printf(" to a normal directory.\n");
		if(tup_db_set_type(dtent, TUP_NODE_DIR) < 0)
			return -1;
		tup_db_del_ghost_tree(dtent);
		dtent = dtent->parent;
	}
	return 0;
}

tupid_t tup_file_mod(tupid_t dt, const char *file, int *modified)
{
	struct stat buf;

	if(tup_db_chdir(dt) < 0)
		return -1;
	if(lstat(file, &buf) != 0) {
		if(errno == ENOENT) {
			return tup_file_del(dt, file, -1, modified);
		}
		fprintf(stderr, "tup error: tup_file_mod() lstat failed.\n");
		perror(file);
		return -1;
	}
	if(fchdir(tup_top_fd()) < 0) {
		perror("fchdir");
		return -1;
	}
	return tup_file_mod_mtime(dt, file, MTIME(buf), 1, 1, modified);
}

tupid_t tup_file_mod_mtime(tupid_t dt, const char *file, struct timespec mtime,
			   int force, int ignore_generated, int *modified)
{
	struct tup_entry *tent;
	struct tup_entry *dtent;
	int new = 0;
	int changed = 0;

	if(tup_entry_add(dt, &dtent) < 0)
		return -1;
	if(tup_db_select_tent(dtent, file, &tent) < 0)
		return -1;

	if(!tent) {
		if(create_name_file(dt, file, mtime, &tent) < 0)
			return -1;
		log_debug_tent("Create", tent, ", mtime=%li.%li\n", mtime.tv_sec, mtime.tv_nsec);
		new = 1;
	} else {
		/* If we are ignoring generated files (ie: from the monitor when it catches
		 * an event from the updater creating output files), then disable force.
		 * In this case we only want to mark the generated files again if the user
		 * is actually changing them, which would trigger the mtime logic.
		 */
		if(ignore_generated && tent->type == TUP_NODE_GENERATED)
			force = 0;
		if(!MTIME_EQ(tent->mtime, mtime) || force) {
			log_debug_tent("Update", tent, ", oldmtime=%li.%li, newmtime=%li.%li, force=%i\n", tent->mtime.tv_sec, tent->mtime.tv_nsec, mtime.tv_sec, mtime.tv_nsec, force);
			changed = 1;
		}

		if(tent->type == TUP_NODE_GHOST) {
			log_debug_tent("Create(overwrite ghost)", tent, "\n");
			if(ghost_to_file(tent) < 0)
				return -1;
		} else if(tent->type != TUP_NODE_FILE &&
			  tent->type != TUP_NODE_GENERATED) {
			log_debug_tent("Create(overwrite)", tent, ", oldtype=%i\n", tent->type);
			if(tup_del_id_type(tent->tnode.tupid, tent->type, 1, NULL) < 0)
				return -1;
			if(tup_db_select_tent(dtent, file, &tent) < 0)
				return -1;
			if(!tent) {
				if(create_name_file(dt, file, mtime, &tent) < 0)
					return -1;
				new = 1;
			} else {
				if(tent->type == TUP_NODE_GHOST) {
					if(ghost_to_file(tent) < 0)
						return -1;
				} else {
					fprintf(stderr, "tup internal error: After attempting to delete node '");
					print_tup_entry(stderr, tent);
					fprintf(stderr, "', it still exists as type '%s'\n", tup_db_type(tent->type));
					return -1;
				}
			}
		}
		if(changed) {
			if(tent->type == TUP_NODE_GENERATED) {
				int tmp = 0;
				if(tup_db_modify_cmds_by_output(tent->tnode.tupid, &tmp) < 0)
					return -1;
				if(tmp == 1) {
					fprintf(stderr, "tup warning: generated file '");
					print_tup_entry(stderr, tent);
					fprintf(stderr, "' was modified outside of tup. This file will be overwritten on the next update, unless the rule that creates it is also removed.\n");
				}
			}
			if(tup_db_add_modify_list(tent->tnode.tupid) < 0)
				return -1;

			if(tup_db_set_dependent_flags(tent->tnode.tupid) < 0)
				return -1;

			if(!MTIME_EQ(tent->mtime, mtime))
				if(tup_db_set_mtime(tent, mtime) < 0)
					return -1;
		}
	}

	if(new || changed) {
		if(modified) *modified = 1;
		if(strcmp(file, TUP_CONFIG) == 0) {
			/* tup.config only counts if it's at the project root, or if
			 * it's in a top-level subdirectory for a variant.
			 */
			if(tent->dt == DOT_DT || tent->parent->dt == DOT_DT) {
				/* If tup.config was modified, put the node in
				 * the config list so we can import any
				 * variables that have changed.
				 */
				if(tup_db_add_config_list(tent->tnode.tupid) < 0)
					return -1;
			}
		}
	}

	return tent->tnode.tupid;
}

static int check_rm_tup_config(struct tup_entry *tent, int *dont_delete)
{
	*dont_delete = 0;
	if(strcmp(tent->name.s, TUP_CONFIG) == 0) {
		/* Just go back to a ghost tup.config node, and add it to the
		 * config list so we can update all of the variables, and clean
		 * up the variant if necessary.
		 */
		*dont_delete = 1;
		if(tup_db_set_type(tent, TUP_NODE_GHOST) < 0)
			return -1;
		if(tup_db_add_config_list(tent->tnode.tupid) < 0)
			return -1;
	}
	return 0;
}

int tup_file_del(tupid_t dt, const char *file, int len, int *modified)
{
	struct tup_entry *tent;
	struct tup_entry *dtent;

	if(tup_entry_add(dt, &dtent) < 0)
		return -1;
	if(len < 0)
		len = strlen(file);

	if(tup_db_select_tent_part(dtent, file, len, &tent) < 0)
		return -1;
	if(!tent) {
		/* If we are trying to delete a file that isn't in tup, that's
		 * probably ok. This can happen if we create and delete a file
		 * real quick before the monitor can create the tup entry
		 * (t7037).
		 */
		return 0;
	}

	/* If .gitignore is removed, make sure we re-parse the Tupfile
	 * (t7040).
	 */
	if(strncmp(file, ".gitignore", len) == 0 && len == 10) {
		if(tup_db_add_create_list(dt) < 0)
			return -1;
	}
	return tup_del_id_type(tent->tnode.tupid, tent->type, 0, modified);
}

int tup_file_missing(struct tup_entry *tent)
{
	int force = 0;
	struct variant *variant;

	variant = tup_entry_variant_null(tent);
	if(variant && !variant->root_variant) {
		if(variant->dtnode.tupid == tent->tnode.tupid) {
			/* Variant root directories use a force removal so that we
			 * don't try to reparse everything.
			 */
			force = 1;
		} else if(tent->type == TUP_NODE_DIR) {
			/* Variant sub-directories get a warning that they will
			 * be re-created.
			 */
			fprintf(stderr, "tup warning: variant directory '");
			print_tup_entry(stderr, tent);
			fprintf(stderr, "' was deleted outside of tup. This directory will be re-created, unless the corresponding source directory was also removed.\n");
		}
	}
	return tup_del_id_type(tent->tnode.tupid, tent->type, force, NULL);
}

int tup_del_id_force(tupid_t tupid, enum TUP_NODE_TYPE type)
{
	return tup_del_id_type(tupid, type, 1, NULL);
}

void tup_register_rmdir_callback(void (*callback)(tupid_t tupid))
{
	rmdir_callback = callback;
}

/* Find the tup_entry equivalent to srctent in the given variant.  Eg: srctent
 * = foo/, variant = build-debug/, then it returns the tent for
 * build-debug/foo.
 */
static struct tup_entry *get_variant_tent(struct tup_entry *srctent, struct variant *variant)
{
	struct tup_entry *parent_tent;
	struct tup_entry *variant_tent;

	if(srctent->tnode.tupid == DOT_DT) {
		return variant->tent->parent;
	}

	parent_tent = get_variant_tent(srctent->parent, variant);
	if(!parent_tent) {
		return NULL;
	}
	if(tup_db_select_tent(parent_tent, srctent->name.s, &variant_tent) < 0) {
		return NULL;
	}
	return variant_tent;
}

int tup_del_id_type(tupid_t tupid, enum TUP_NODE_TYPE type, int force, int *modified)
{
	struct tup_entry *tent;
	int dont_delete = 0;

	if(tup_entry_add(tupid, &tent) < 0)
		return -1;
	log_debug_tent("Delete", tent, ", type=%i, force=%i\n", type, force);

	if(check_rm_tup_config(tent, &dont_delete) < 0)
		return -1;
	if(dont_delete)
		return 0;

	if(type == TUP_NODE_GHOST) {
		/* Don't want to delete ghosts, since they may still
		 * link to somewhere useful (t6061)
		 */
		return 0;
	}
	if(type == TUP_NODE_GROUP) {
		/* We don't delete groups here - they are reclaimed similar to
		 * ghosts (t3078)
		 */
		return 0;
	}

	if(type == TUP_NODE_GENERATED_DIR) {
		if(tup_db_flag_generated_dir(tupid, force) < 0)
			return -1;
		if(rmdir_callback)
			rmdir_callback(tupid);
		return 0;
	}

	if(type == TUP_NODE_DIR) {
		struct variant *variant;

		/* Recurse and kill anything below this dir. Note that
		 * tup_db_delete_dir() calls back to this function.
		 */
		if(tup_db_delete_dir(tupid, force) < 0)
			return -1;
		if(rmdir_callback)
			rmdir_callback(tupid);

		/* Try to figure out if we are a variant directory - if so, we
		 * may need to reparse the src directory to try to re-create
		 * the variant dir. We use tup_entry_variant_null here since
		 * the root variant may not be created yet. We only try to do
		 * this if the scanner/monitor detects a missing file, not if
		 * the updater deletes the variant directory because the src
		 * directory was already deleted.
		 */
		variant = tup_entry_variant_null(tent);
		if(variant && !variant->root_variant) {
			if(variant->enabled && !force) {
				/* It is possible that the srcid has already
				 * been removed (ie: The user rm -rf'd the
				 * variant, and the corresponding source
				 * directory). If the source directory was
				 * missing first, then its node has been
				 * removed from the database. Adding it to the
				 * create list would confuse tup, so we use the
				 * 'maybe' version here to make sure the node
				 * exists before adding it (t8035).
				 */
				if(tup_db_maybe_add_create_list(tent->srcid) < 0)
					return -1;
			}
		} else {
			/* If we are removing a directory in the srctree that
			 * has a ghost Tupfile, make sure we notify all of the
			 * variant directories to be re-parsed. This way they
			 * can be cleaned up as necessary (t8020).
			 */
			struct tup_entry *tuptent;
			if(tup_db_select_tent(tent, "Tupfile", &tuptent) < 0)
				return -1;
			if(tuptent) {
				if(tup_db_set_dependent_dir_flags(tuptent->tnode.tupid) < 0)
					return -1;
			}
			LIST_FOREACH(variant, get_variant_list(), list) {
				if(!variant->root_variant) {
					struct tup_entry *variant_tent;
					variant_tent = get_variant_tent(tent, variant);
					if(variant_tent) {
						if(tup_db_set_srcid(variant_tent, VARIANT_SRCDIR_REMOVED) < 0)
							return -1;
					}
				}
			}

			/* Flag our parent directory in case it needs to become
			 * a generated directory (t4124)
			 */
			if(tent->parent->type == TUP_NODE_DIR) {
				if(tup_db_add_create_list(tent->dt) < 0)
					return -1;
			}
		}
	}

	/* If a file was deleted and it was created by a command, set the
	 * command's flags to modify. For example, if foo.o was deleted, we set
	 * 'gcc -c foo.c -o foo.o' to modify, so it will be re-executed. This
	 * only happens if a file was deleted outside of the parser (!force).
	 */
	if(type == TUP_NODE_GENERATED && !force) {
		int changed = 0;

		/* If a generated.gitignore file was removed, re-parse
		 * the directory so it will be recreated.
		 */
		if(strcmp(tent->name.s, ".gitignore") == 0) {
			if(tup_db_add_create_list(tent->dt) < 0)
				return -1;
			return 0;
		}

		if(tup_db_modify_cmds_by_output(tupid, &changed) < 0)
			return -1;

		/* Since the file has been removed, make sure it is no longer
		 * in the modify list (t5071)
		 */
		if(tup_db_unflag_modify(tupid) < 0)
			return -1;

		/* Transient files don't need a warning, since tup likely was
		 * the one who deleted them.
		 */
		if(is_transient_tent(tent)) {
			return 0;
		}

		/* Only display a warning if the command isn't already in the
		 * modify list. It's possible that the command hasn't actually
		 * been executed yet.
		 */
		if(changed == 1) {
			fprintf(stderr, "tup warning: generated file '");
			print_tup_entry(stderr, tent);
			fprintf(stderr, "' was deleted outside of tup. This file may be re-created on the next update.\n");
			if(modified) *modified = 1;
		}

		/* If we're not forcing the deletion, just return here (the
		 * node won't actually be removed from tup). The fact that the
		 * command is in modify will take care of dependencies, and
		 * we don't want to put the directory back in create (t6036).
		 */
		return 0;
	}
	if(modified) *modified = 1;

	if(type == TUP_NODE_FILE || type == TUP_NODE_DIR) {
		if(tup_db_set_dependent_flags(tupid) < 0)
			return -1;
	}

	if(type == TUP_NODE_FILE || type == TUP_NODE_GENERATED) {
		/* We also have to run any command that used this file as an
		 * input, so we can yell at the user if they haven't already
		 * fixed that command.
		 */
		if(tup_db_modify_cmds_by_input(tupid) < 0)
			return -1;

		if(!force) {
			/* Re-parse the current Tupfile (the updater
			 * automatically parses any dependent directories).
			 */
			if(tup_db_add_create_list(tent->dt) < 0)
				return -1;
		}
	}
	if(delete_name_file(tupid) < 0)
		return -1;
	return 0;
}

struct tup_entry *get_tent_dt(tupid_t dt, const char *path)
{
	struct path_element *pel = NULL;
	struct tup_entry *tent;
	struct tup_entry *dtent;

	dt = find_dir_tupid_dt(dt, path, &pel, 0, 1);
	if(dt <= 0)
		return NULL;

	if(tup_entry_add(dt, &dtent) < 0)
		return NULL;

	if(pel) {
		if(tup_db_select_tent_part(dtent, pel->path, pel->len, &tent) < 0)
			return NULL;
		free_pel(pel);
		if(!tent)
			return NULL;
		return tent;
	} else {
		/* We get here if the path list ends up being empty (for
		 * example, if the path is ".")
		 */
		return tup_entry_get(dt);
	}
}

tupid_t find_dir_tupid(const char *dir)
{
	struct tup_entry *tent;
	tupid_t dt = DOT_DT;

	/* This check is used for tests to get the parent tupid for the '.'
	 * directory.
	 */
	if(strcmp(dir, "0") == 0)
		return 0;
	if(strcmp(dir, "/") == 0)
		return slash_dt();
	tent = get_tent_dt(dt, dir);
	if(!tent)
		return -1;
	return tent->tnode.tupid;
}

tupid_t find_dir_tupid_dt(tupid_t dt, const char *dir,
			  struct path_element **last, int sotgv, int full_deps)
{
	struct pel_group pg;
	tupid_t tupid;

	if(get_path_elements(dir, &pg) < 0)
		return -1;

	tupid = find_dir_tupid_dt_pg(dt, &pg, last, sotgv, full_deps);
	return tupid;
}

tupid_t find_dir_tupid_dt_pg(tupid_t dt, struct pel_group *pg,
			     struct path_element **last, int sotgv, int full_deps)
{
	struct path_element *pel;
	struct tup_entry *tent;

	/* Ignore if the file is hidden */
	if(pg->pg_flags & PG_HIDDEN)
		return 0;

	/* If we aren't in full deps mode and the file is outside tup, we ignore it */
	if(!full_deps && (pg->pg_flags & PG_OUTSIDE_TUP)) {
		return 0;
	}

	/* The list can be empty if dir is "." or something like "foo/..". In
	 * this case just return dt (the start dir).
	 */
	if(TAILQ_EMPTY(&pg->path_list)) {
		if(tup_entry_add(dt, &tent) < 0)
			return -1;
		return dt;
	}

	if(last) {
		pel = TAILQ_LAST(&pg->path_list, path_element_head);
		*last = pel;
		TAILQ_REMOVE(&pg->path_list, pel, list);
	}

	if(pg->pg_flags & PG_ROOT)
		dt = 1;
	if(pg->pg_flags & PG_OUTSIDE_TUP)
		dt = slash_dt();

	if(tup_entry_add(dt, &tent) < 0)
		return -1;

	while(!TAILQ_EMPTY(&pg->path_list)) {
		pel = TAILQ_FIRST(&pg->path_list);
		if(pel->len == 2 && pel->path[0] == '.' && pel->path[1] == '.') {
			if(tent->parent == NULL) {
				/* If we're at the top of the tup hierarchy and
				 * trying to go up a level, bail out and return
				 * success since we don't keep track of files
				 * in the great beyond.
				 */
				if(last) {
					free_pel(*last);
					*last = NULL;
				}
				del_pel_group(pg);
				return 0;
			}
			tent = tent->parent;
		} else {
			struct tup_entry *curtent = tent;

			if(tup_db_select_tent_part(tent, pel->path, pel->len, &tent) < 0)
				return -1;
			if(tent) {
				if(sotgv == SOTGV_CREATE_DIRS) {
					if(tent->type == TUP_NODE_GHOST) {
						if(tup_db_set_type(tent, TUP_NODE_GENERATED_DIR) < 0)
							return -1;
						if(tup_db_add_modify_list(tent->tnode.tupid) < 0)
							return -1;
					} else if(tent->type != TUP_NODE_DIR &&
						  tent->type != TUP_NODE_GENERATED_DIR) {
						fprintf(stderr, "tup error: Unable to output to a different directory because '");
						print_tup_entry(stderr, tent);
						fprintf(stderr, "' is a %s\n", tup_db_type(tent->type));
						return -1;
					}
				}
			} else {
				int type = TUP_NODE_GHOST;
				struct timespec mtime = {-1, 0};

				/* Secret of the ghost valley! */
				if(sotgv == 0) {
					return -1;
				}
				if(sotgv == SOTGV_CREATE_DIRS)
					type = TUP_NODE_GENERATED_DIR;
				else if(sotgv == SOTGV_IGNORE_DIRS && !(pg->pg_flags & PG_OUTSIDE_TUP))
					type = TUP_NODE_DIR;

				if(full_deps && (pg->pg_flags & PG_OUTSIDE_TUP)) {
					if(get_outside_tup_mtime(curtent, pel, &mtime) < 0)
						return -1;
				}
				if(tup_db_node_insert_tent(curtent, pel->path, pel->len, type, mtime, -1, &tent) < 0)
					return -1;
			}
		}

		del_pel(pel, pg);
	}

	return tent->tnode.tupid;
}

int get_outside_tup_mtime(struct tup_entry *parent, struct path_element *pel, struct timespec *mtime)
{
	int dfd;

	dfd = tup_entry_open(parent);
	if(dfd == -ENOENT || dfd == -ENOTDIR) {
		*mtime = INVALID_MTIME;
	} else if(dfd < 0) {
		perror("tup_entry_open");
		fprintf(stderr, "tup error: Unable to open tup entry: ");
		print_tup_entry(stderr, parent);
		fprintf(stderr, "\n");
		return -1;
	} else {
		struct stat buf;
		char tmppath[PATH_MAX];

		/* Pel's aren't nul-terminated, so unfortunately we have to
		 * make a copy here.
		 */
		strncpy(tmppath, pel->path, pel->len);
		tmppath[pel->len] = 0;
		if(fstatat(dfd, tmppath, &buf, AT_SYMLINK_NOFOLLOW) < 0) {
			if(errno != ENOENT && errno != ENOTDIR) {
				perror("fstatat");
				fprintf(stderr, "tup error: Unable to stat file: %.*s\n", pel->len, pel->path);
				return -1;
			}
			*mtime = INVALID_MTIME;
		} else {
			/* Ghost directories in the /-tree have mtimes set to
			 * zero if they exist. This way we can distinguish
			 * between a directory being created where there wasn't
			 * one previously (t4064, t4205).
			 */
			if(S_ISDIR(buf.st_mode))
				*mtime = EXTERNAL_DIRECTORY_MTIME;
			else
				*mtime = MTIME(buf);
		}
		if(close(dfd) < 0) {
			perror("close(dfd)");
			return -1;
		}
	}
	return 0;
}

int gimme_tent(const char *name, struct tup_entry **entry)
{
	tupid_t dt;
	struct tup_entry *dtent;
	struct path_element *pel = NULL;

	dt = find_dir_tupid_dt(DOT_DT, name, &pel, 0, 1);
	if(dt < 0)
		return -1;
	if(dt == 0) {
		*entry = NULL;
		return 0;
	}
	if(pel == NULL) {
		*entry = tup_entry_get(dt);
		return 0;
	}
	if(tup_entry_add(dt, &dtent) < 0)
		return -1;
	if(tup_db_select_tent_part(dtent, pel->path, pel->len, entry) < 0)
		return -1;
	free_pel(pel);
	return 0;
}

static int ghost_to_file(struct tup_entry *tent)
{
	tup_db_del_ghost_tree(tent);
	if(tup_db_set_type(tent, TUP_NODE_FILE) < 0)
		return -1;
	/* Only add dirs, not generated dirs, to the create list. */
	if(tent->parent->type == TUP_NODE_DIR)
		if(tup_db_add_create_list(tent->dt) < 0)
			return -1;
	if(tup_db_add_modify_list(tent->tnode.tupid) < 0)
		return -1;
	return 0;
}
