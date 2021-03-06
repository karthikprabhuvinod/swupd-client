/*
*   Software Updater - client side
*
*      Copyright (c) 2012-2016 Intel Corporation.
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, version 2 or later of the License.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Authors:
*         Jaime A. Garcia <jaime.garcia.naranjo@linux.intel.com>
*         Tim Pepper <timothy.c.pepper@linux.intel.com>
*
*/

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "alias.h"
#include "config.h"
#include "swupd.h"

#define MODE_RW_O (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

static bool cmdline_option_force = false;

void remove_set_option_force(bool opt)
{
	cmdline_option_force = opt;
}

/*
* list_installable_bundles()
* Parse the full manifest for the current version of the OS and print
*   all available bundles.
*/
enum swupd_code list_installable_bundles()
{
	char *name;
	struct list *list;
	struct file *file;
	struct manifest *MoM = NULL;
	int current_version;
	bool mix_exists;

	current_version = get_current_version(globals.path_prefix);
	if (current_version < 0) {
		error("Unable to determine current OS version\n");
		return SWUPD_CURRENT_VERSION_UNKNOWN;
	}

	mix_exists = (check_mix_exists() & system_on_mix());
	MoM = load_mom(current_version, mix_exists, NULL);
	if (!MoM) {
		return SWUPD_COULDNT_LOAD_MOM;
	}

	list = MoM->manifests = list_sort(MoM->manifests, file_sort_filename);
	while (list) {
		file = list->data;
		list = list->next;
		name = get_printable_bundle_name(file->filename, file->is_experimental);
		print("%s\n", name);
		free_string(&name);
	}

	free_manifest(MoM);
	return 0;
}

/* Finds out whether bundle_name is installed bundle on
*  current system.
*/
bool is_installed_bundle(const char *bundle_name)
{
	struct stat statb;
	char *filename = NULL;
	bool ret = true;

	string_or_die(&filename, "%s/%s/%s", globals.path_prefix, BUNDLES_DIR, bundle_name);

	if (stat(filename, &statb) == -1) {
		ret = false;
	}

	free_string(&filename);
	return ret;
}

static int find_manifest(const void *a, const void *b)
{
	struct manifest *A;
	char *B;
	int ret;

	A = (struct manifest *)a;
	B = (char *)b;

	ret = strcmp(A->component, B);
	if (ret != 0) {
		return ret;
	}

	/* we found a match*/
	return 0;
}

/* Return list of bundles that include bundle_name */
static int required_by(struct list **reqd_by, const char *bundle_name, struct manifest *mom, int recursion, struct list *exclusions, char *msg)
{
	struct list *b, *i;
	char *name;
	int count = 0;
	static bool print_msg;
	bool verbose = (log_get_level() == LOG_INFO_VERBOSE);

	// track recursion level for indentation
	if (recursion == 0) {
		print_msg = true;
	}
	recursion++;

	/* look at the manifest of all listed bundles to see if
	 * they list *bundle_name as a dependency */
	b = list_head(mom->submanifests);
	while (b) {
		struct manifest *bundle = b->data;
		b = b->next;

		if (strcmp(bundle->component, bundle_name) == 0) {
			/* circular dependencies are not allowed in manifests,
			 * so we can skip checking for dependencies in the same bundle */
			continue;
		}

		int indent = 0;
		i = list_head(bundle->includes);
		while (i) {
			name = i->data;
			i = i->next;

			if (strcmp(name, bundle_name) == 0) {

				/* this bundle has *bundle_name as a dependency */

				/* if the bundle being looked at is in the list of exclusions
				 * then don't consider it as a dependency, the user added it to
				 * the list of bundles to be removed too, but we DO want to
				 * consider its list of includes */
				if (!list_search(exclusions, bundle->component, list_strcmp)) {

					/* add bundle to list of dependencies */
					char *bundle_str = NULL;
					string_or_die(&bundle_str, "%s", bundle->component);
					*reqd_by = list_append_data(*reqd_by, bundle_str);

					/* if the --verbose options was used, print the dependency as a
					 * tree element, in this view it is ok to have duplicated elements */
					if (verbose) {
						if (print_msg) {
							/* these messages should be printed only once */
							print_msg = false;
							info("%s", msg);
							info("\nformat:\n");
							info(" # * is-required-by\n");
							info(" #   |-- is-required-by\n");
							info(" # * is-also-required-by\n # ...\n");
							info("\n");
						}
						indent = (recursion - 1) * 4;
						if (recursion == 1) {
							info("%*s* %s\n", indent + 2, "", bundle->component);
						} else {
							info("%*s|-- %s\n", indent, "", bundle->component);
						}
					}
				}

				/* let's see what bundles list this new bundle as a dependency */
				required_by(reqd_by, bundle->component, mom, recursion, exclusions, msg);
			}
		}
	}

	if (recursion == 1) {
		/* get rid of duplicated dependencies */
		*reqd_by = list_str_deduplicate(*reqd_by);

		/* if not using --verbose, we need to print the simplified
		 * list of bundles that depend on *bundle_name */
		i = list_head(*reqd_by);
		while (i) {
			name = i->data;
			count++;
			i = i->next;

			if (!verbose) {
				if (print_msg) {
					/* this message should be printed only once */
					print_msg = false;
					info("%s", msg);
				}
				info(" - %s\n", name);
			}
		}
	}

	return count;
}

/* Return recursive list of included bundles */
enum swupd_code show_included_bundles(char *bundle_name)
{
	int ret = 0;
	int current_version = CURRENT_OS_VERSION;
	struct list *subs = NULL;
	struct list *deps = NULL;
	struct manifest *mom = NULL;

	current_version = get_current_version(globals.path_prefix);
	if (current_version < 0) {
		error("Unable to determine current OS version\n");
		ret = SWUPD_CURRENT_VERSION_UNKNOWN;
		goto out;
	}

	mom = load_mom(current_version, false, NULL);
	if (!mom) {
		error("Cannot load official manifest MoM for version %i\n", current_version);
		ret = SWUPD_COULDNT_LOAD_MOM;
		goto out;
	}

	// add_subscriptions takes a list, so construct one with only bundle_name
	struct list *bundles = NULL;
	bundles = list_prepend_data(bundles, bundle_name);
	ret = add_subscriptions(bundles, &subs, mom, true, 0);
	list_free_list(bundles);
	if (ret != add_sub_NEW) {
		// something went wrong or there were no includes, print a message and exit
		char *m = NULL;
		if (ret & add_sub_ERR) {
			string_or_die(&m, "Processing error");
			ret = SWUPD_COULDNT_LOAD_MANIFEST;
		} else if (ret & add_sub_BADNAME) {
			string_or_die(&m, "Bad bundle name detected");
			ret = SWUPD_INVALID_BUNDLE;
		} else {
			string_or_die(&m, "Unknown error");
			ret = SWUPD_UNEXPECTED_CONDITION;
		}

		error("%s - Aborting\n", m);
		free_string(&m);
		goto out;
	}
	deps = recurse_manifest(mom, subs, NULL, false, NULL);
	if (!deps) {
		error("Cannot load included bundles\n");
		ret = SWUPD_RECURSE_MANIFEST;
		goto out;
	}

	/* deps now includes the bundle indicated by bundle_name
	 * if deps only has one bundle in it, no included packages were found */
	if (list_len(deps) == 1) {
		info("No included bundles\n");
		ret = SWUPD_OK;
		goto out;
	}

	info("Bundles included by %s:\n\n", bundle_name);

	struct list *iter;
	iter = list_head(deps);
	while (iter) {
		struct manifest *included_bundle = iter->data;
		iter = iter->next;
		// deps includes the bundle_name bundle, skip it
		if (strcmp(bundle_name, included_bundle->component) == 0) {
			continue;
		}

		print("%s\n", included_bundle->component);
	}

	ret = SWUPD_OK;

out:
	if (mom) {
		free_manifest(mom);
	}

	if (deps) {
		list_free_list_and_data(deps, free_manifest_data);
	}

	if (subs) {
		free_subscriptions(&subs);
	}

	return ret;
}

enum swupd_code show_bundle_reqd_by(const char *bundle_name, bool server)
{
	int ret = 0;
	int version = CURRENT_OS_VERSION;
	struct manifest *current_manifest = NULL;
	struct list *subs = NULL;
	struct list *reqd_by = NULL;
	int number_of_reqd = 0;

	if (!server && !is_installed_bundle(bundle_name)) {
		info("Bundle \"%s\" does not seem to be installed\n", bundle_name);
		info("       try passing --all to check uninstalled bundles\n");
		ret = SWUPD_BUNDLE_NOT_TRACKED;
		goto out;
	}

	version = get_current_version(globals.path_prefix);
	if (version < 0) {
		error("Unable to determine current OS version\n");
		ret = SWUPD_CURRENT_VERSION_UNKNOWN;
		goto out;
	}

	current_manifest = load_mom(version, false, NULL);
	if (!current_manifest) {
		error("Unable to download/verify %d Manifest.MoM\n", version);
		ret = SWUPD_COULDNT_LOAD_MOM;
		goto out;
	}

	if (!search_bundle_in_manifest(current_manifest, bundle_name)) {
		error("Bundle \"%s\" is invalid, aborting dependency list\n", bundle_name);
		ret = SWUPD_INVALID_BUNDLE;
		goto out;
	}

	if (server) {
		ret = add_included_manifests(current_manifest, &subs);
		if (ret) {
			error("Unable to load server manifest");
			ret = SWUPD_COULDNT_LOAD_MANIFEST;
			goto out;
		}

	} else {
		/* load all tracked bundles into memory */
		read_subscriptions(&subs);
	}

	/* load all submanifests */
	current_manifest->submanifests = recurse_manifest(current_manifest, subs, NULL, server, NULL);
	if (!current_manifest->submanifests) {
		error("Cannot load MoM sub-manifests\n");
		ret = SWUPD_RECURSE_MANIFEST;
		goto out;
	}

	char *msg;
	string_or_die(&msg, "%s bundles that have %s as a dependency:\n", server ? "All installable and installed" : "Installed", bundle_name);
	number_of_reqd = required_by(&reqd_by, bundle_name, current_manifest, 0, NULL, msg);
	free_string(&msg);
	if (reqd_by == NULL) {
		info("No bundles have %s as a dependency\n", bundle_name);
		ret = SWUPD_OK;
		goto out;
	}
	list_free_list_and_data(reqd_by, free);
	info("\nBundle '%s' is required by %d bundle%s\n", bundle_name, number_of_reqd, number_of_reqd == 1 ? "" : "s");

	ret = SWUPD_OK;

out:
	if (current_manifest) {
		free_manifest(current_manifest);
	}

	if (ret) {
		print("Bundle list failed\n");
	}

	if (subs) {
		free_subscriptions(&subs);
	}

	return ret;
}

static char *tracking_dir(void)
{
	return mk_full_filename(globals.state_dir, "bundles");
}

/*
 * remove_tracked removes the tracking file in
 * path_prefix/state_dir_parent/bundles if it exists to untrack as manually
 * installed if the file exists
 */
static void remove_tracked(const char *bundle)
{
	char *destdir = tracking_dir();
	char *tracking_file = mk_full_filename(destdir, bundle);
	free_string(&destdir);
	/* we don't care about failures here since any weird state in the tracking
	 * dir MUST be handled gracefully */
	swupd_rm(tracking_file);
	free_string(&tracking_file);
}

/*
 * track_installed creates a tracking file in path_prefix/var/lib/bundles If
 * there are no tracked files in that directory (directory is empty or does not
 * exist) copy the tracking directory at path_prefix/usr/share/clear/bundles to
 * path_prefix/var/lib/bundles to initiate the tracking files.
 *
 * This function does not return an error code because weird state in this
 * directory must be handled gracefully whenever encountered.
 */
static void track_installed(const char *bundle_name)
{
	int ret = 0;
	char *dst = tracking_dir();
	char *src;

	/* if state_dir_parent/bundles doesn't exist or is empty, assume this is
	 * the first time tracking installed bundles. Since we don't know what the
	 * user installed themselves just copy the entire system tracking directory
	 * into the state tracking directory. */
	if (!is_populated_dir(dst)) {
		char *rmfile;
		ret = rm_rf(dst);
		if (ret) {
			goto out;
		}
		src = mk_full_filename(globals.path_prefix, "/usr/share/clear/bundles");
		/* at the point this function is called <bundle_name> is already
		 * installed on the system and therefore has a tracking file under
		 * /usr/share/clear/bundles. A simple cp -a of that directory will
		 * accurately track that bundle as manually installed. */
		ret = copy_all(src, globals.state_dir);
		free_string(&src);
		if (ret) {
			goto out;
		}
		/* remove uglies that live in the system tracking directory */
		rmfile = mk_full_filename(dst, ".MoM");
		(void)unlink(rmfile);
		free_string(&rmfile);
		/* set perms on the directory correctly */
		ret = chmod(dst, S_IRWXU);
		if (ret) {
			goto out;
		}
	}

	char *tracking_file = mk_full_filename(dst, bundle_name);
	int fd = open(tracking_file, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	free_string(&tracking_file);
	if (fd < 0) {
		ret = -1;
		goto out;
	}
	close(fd);

out:
	if (ret) {
		debug("Issue creating tracking file in %s for %s\n", dst, bundle_name);
	}
	free_string(&dst);
}

static int filter_files_to_delete(const void *a, const void *b)
{
	struct file *A, *B;
	int ret;

	/* matched items will be removed from the list of files to be deleted */

	A = (struct file *)a;
	B = (struct file *)b;

	/* if the file we are looking at is marked as already deleted
	 * it can be removed from the list, so return a match */
	if (A->is_deleted) {
		return 0;
	}

	ret = strcmp(A->filename, B->filename);
	if (ret) {
		return ret;
	}

	/* if the file is marked as not deleted in B, the file is still
	 * needed in the system, so return a match */
	if (!B->is_deleted) {
		return 0;
	}

	return -1;
}

/*  This function is a fresh new implementation for a bundle
 *  remove without being tied to verify loop, this means
 *  improved speed and space as well as more robustness and
 *  flexibility. The function removes one or more bundles
 *  passed in the bundles param.
 *
 *  For each one of the bundles to be removed what it does
 *  is basically:
 *
 *  1) Read MoM and load all submanifests except the one to be
 *  	removed and then consolidate them.
 *  2) Load the removed bundle submanifest.
 *  3) Order the file list by filename
 *  4) Deduplicate removed submanifest file list that happens
 *  	to be on the MoM (minus bundle to be removed).
 *  5) iterate over to be removed bundle submanifest file list
 *  	performing a unlink(2) for each filename.
 *  6) Done.
 */
enum swupd_code remove_bundles(struct list *bundles)
{
	int ret = SWUPD_OK;
	int ret_code = 0;
	int bad = 0;
	int total = 0;
	int current_version = CURRENT_OS_VERSION;
	struct manifest *current_mom = NULL;
	struct list *subs = NULL;
	struct list *bundles_to_remove = NULL;
	struct list *files_to_remove = NULL;
	struct list *reqd_by = NULL;
	char *bundles_list_str = NULL;
	bool mix_exists;

	ret = swupd_init(SWUPD_ALL);
	if (ret != 0) {
		error("Failed updater initialization, exiting now\n");
		return ret;
	}

	current_version = get_current_version(globals.path_prefix);
	if (current_version < 0) {
		error("Unable to determine current OS version\n");
		ret_code = SWUPD_CURRENT_VERSION_UNKNOWN;
		goto out_deinit;
	}

	mix_exists = (check_mix_exists() & system_on_mix());

	current_mom = load_mom(current_version, mix_exists, NULL);
	if (!current_mom) {
		error("Unable to download/verify %d Manifest.MoM\n", current_version);
		ret_code = SWUPD_COULDNT_LOAD_MOM;
		goto out_deinit;
	}

	/* load all tracked bundles into memory */
	read_subscriptions(&subs);
	set_subscription_versions(current_mom, NULL, &subs);

	/* load all submanifests */
	current_mom->submanifests = recurse_manifest(current_mom, subs, NULL, false, NULL);
	if (!current_mom->submanifests) {
		error("Cannot load MoM sub-manifests\n");
		ret_code = SWUPD_RECURSE_MANIFEST;
		goto out_subs;
	}

	for (; bundles; bundles = bundles->next, total++) {

		char *bundle = bundles->data;

		/* os-core bundle not allowed to be removed...
		* although this is going to be caught later because of all files
		* being marked as 'duplicated' and note removing anything
		* anyways, better catch here and return success, no extra work to be done.
		*/
		if (strcmp(bundle, "os-core") == 0) {
			warn("\nBundle \"os-core\" not allowed to be removed, skipping it...\n");
			ret_code = SWUPD_REQUIRED_BUNDLE_ERROR;
			bad++;
			continue;
		}

		if (!search_bundle_in_manifest(current_mom, bundle)) {
			warn("\nBundle \"%s\" is invalid, skipping it...\n", bundle);
			ret_code = SWUPD_INVALID_BUNDLE;
			bad++;
			continue;
		}

		if (!is_installed_bundle(bundle)) {
			warn("\nBundle \"%s\" is not installed, skipping it...\n", bundle);
			ret_code = SWUPD_BUNDLE_NOT_TRACKED;
			bad++;
			continue;
		}

		/* check if bundle is required by another installed bundle */
		char *msg;
		string_or_die(&msg, "\nBundle \"%s\" is required by the following bundles:\n", bundle);
		int number_of_reqd = required_by(&reqd_by, bundle, current_mom, 0, bundles, msg);
		free_string(&msg);
		if (number_of_reqd > 0) {
			/* the bundle is required by other bundles, do not continue with the removal
			 * unless the --force flag is used */
			if (!cmdline_option_force) {

				error("\nBundle \"%s\" is required by %d bundle%s, skipping it...\n", bundle, number_of_reqd, number_of_reqd == 1 ? "" : "s");
				info("Use \"swupd bundle-remove --force %s\" to remove \"%s\" and all bundles that require it\n", bundle, bundle);
				ret_code = SWUPD_REQUIRED_BUNDLE_ERROR;
				bad++;
				list_free_list_and_data(reqd_by, free);
				reqd_by = NULL;
				continue;

			} else {

				/* the --force option was specified */
				info("\nThe --force option was used, bundle \"%s\" and all bundles that require it will be removed from the system\n", bundle);

				/* move the manifest of dependent bundles to the list of bundles to be removed */
				struct list *iter = NULL;
				char *dep;
				iter = list_head(reqd_by);
				while (iter) {
					dep = iter->data;
					iter = iter->next;
					list_move_item(dep, &current_mom->submanifests, &bundles_to_remove, find_manifest);
					remove_tracked(dep);
				}
				list_free_list_and_data(reqd_by, free);
				reqd_by = NULL;
			}
		}

		/* move the manifest of the bundle to be removed from the list of subscribed bundles
		 * to the list of bundles to be removed */
		list_move_item(bundle, &current_mom->submanifests, &bundles_to_remove, find_manifest);
		info("\nRemoving bundle: %s\n", bundle);
		remove_tracked(bundle);
	}

	/* if there are no bundles to remove we are done */
	if (bundles_to_remove) {

		/* get the list of all files required by the installed bundles (except the ones to be removed) */
		current_mom->files = consolidate_files_from_bundles(current_mom->submanifests);

		/* get the list of the files contained in the bundles to be removed */
		files_to_remove = consolidate_files_from_bundles(bundles_to_remove);

		/* sanitize files to remove; if a file is needed by a bundle that
		 * is installed, it should be kept in the system */
		files_to_remove = list_filter_common_elements(files_to_remove, current_mom->files, filter_files_to_delete, NULL);

		if (list_len(files_to_remove) > 0) {
			info("\nDeleting bundle files...\n");
			progress_set_step(1, "remove_files");
			int deleted = remove_files_from_fs(files_to_remove);
			info("Total deleted files: %i\n", deleted);
		}
	}

	if (bad > 0) {
		print("\nFailed to remove %i of %i bundles\n", bad, total);
	} else {
		print("\nSuccessfully removed %i bundle%s\n", total, (total > 1 ? "s" : ""));
	}

	list_free_list(files_to_remove);
	list_free_list_and_data(bundles_to_remove, free_manifest_data);
	free_manifest(current_mom);
	free_subscriptions(&subs);
	swupd_deinit();

	return ret_code;

out_subs:
	free_manifest(current_mom);
	free_subscriptions(&subs);
out_deinit:
	bundles_list_str = string_join(", ", bundles);
	telemetry(TELEMETRY_CRIT,
		  "bundleremove",
		  "bundle=%s\n"
		  "current_version=%d\n"
		  "result=%d\n"
		  "bytes=%ld\n",
		  bundles_list_str,
		  current_version,
		  ret_code,
		  total_curl_sz);
	free_string(&bundles_list_str);
	swupd_deinit();
	print("\nFailed to remove bundle(s)\n");

	return ret_code;
}

/* bitmapped return
   1 error happened
   2 new subscriptions
   4 bad name given
*/
int add_subscriptions(struct list *bundles, struct list **subs, struct manifest *mom, bool find_all, int recursion)
{
	char *bundle;
	int manifest_err;
	int ret = 0;
	struct file *file;
	struct list *iter;
	struct manifest *manifest;

	iter = list_head(bundles);
	while (iter) {
		bundle = iter->data;
		iter = iter->next;

		file = search_bundle_in_manifest(mom, bundle);
		if (!file) {
			warn("Bundle \"%s\" is invalid, skipping it...\n", bundle);
			ret |= add_sub_BADNAME; /* Use this to get non-zero exit code */
			continue;
		}

		if (!find_all && is_installed_bundle(bundle)) {
			continue;
		}

		manifest = load_manifest(file->last_change, file, mom, true, &manifest_err);
		if (!manifest) {
			error("Unable to download manifest %s version %d, exiting now\n", bundle, file->last_change);
			ret |= add_sub_ERR;
			goto out;
		}

		/*
		 * If we're recursing a tree of includes, we need to cut out early
		 * if the bundle we're looking at is already subscribed...
		 * Because if it is, we'll visit it soon anyway at the top level.
		 *
		 * We can't do this for the toplevel of the recursion because
		 * that is how we initiallly fill in the include tree.
		 */
		if (component_subscribed(*subs, bundle)) {
			if (recursion > 0) {
				free_manifest(manifest);
				continue;
			}
		} else {
			// Just add it to a list if it doesn't exist
			create_and_append_subscription(subs, bundle);
			ret |= add_sub_NEW; /* We have added at least one */
		}

		if (manifest->includes) {
			/* merge in recursive call results */
			ret |= add_subscriptions(manifest->includes, subs, mom, find_all, recursion + 1);
		}

		if (!globals.skip_optional_bundles && manifest->optional) {
			/* merge in recursive call results */
			ret |= add_subscriptions(manifest->optional, subs, mom, find_all, recursion + 1);
		}

		free_manifest(manifest);
	}
out:
	return ret;
}

static enum swupd_code install_bundles(struct list *bundles, struct list **subs, struct manifest *mom)
{
	int ret;
	int bundles_failed = 0;
	int already_installed = 0;
	int bundles_installed = 0;
	int dependencies_installed = 0;
	int bundles_requested = list_len(bundles);
	long bundle_size = 0;
	long fs_free = 0;
	struct file *file;
	struct list *iter;
	struct list *installed_bundles = NULL;
	struct list *installed_files = NULL;
	struct list *to_install_bundles = NULL;
	struct list *to_install_files = NULL;
	struct list *current_subs = NULL;
	bool invalid_bundle_provided = false;

	/* step 1: get subscriptions for bundles to be installed */
	info("Loading required manifests...\n");
	timelist_timer_start(globals.global_times, "Add bundles and recurse");
	progress_set_step(1, "load_manifests");
	ret = add_subscriptions(bundles, subs, mom, false, 0);

	/* print a message if any of the requested bundles is already installed */
	iter = list_head(bundles);
	while (iter) {
		char *bundle;
		bundle = iter->data;
		iter = iter->next;
		if (is_installed_bundle(bundle)) {
			warn("Bundle \"%s\" is already installed, skipping it...\n", bundle);
			already_installed++;
			/* track as installed since the user tried to install */
			track_installed(bundle);
		}
		/* warn the user if the bundle to be installed is experimental */
		file = search_bundle_in_manifest(mom, bundle);
		if (file && file->is_experimental) {
			warn("Bundle %s is experimental\n", bundle);
		}
	}

	/* Use a bitwise AND with the add_sub_NEW mask to determine if at least
	 * one new bundle was subscribed */
	if (!(ret & add_sub_NEW)) {
		/* something went wrong, nothing will be installed */
		if (ret & add_sub_ERR) {
			ret = SWUPD_COULDNT_LOAD_MANIFEST;
		} else if (ret & add_sub_BADNAME) {
			ret = SWUPD_INVALID_BUNDLE;
		} else {
			/* this means the user tried to add a bundle that
			 * was already installed, nothing to do here */
			ret = SWUPD_OK;
		}
		goto out;
	}
	/* If at least one of the provided bundles was invalid set this flag
	 * so we can check it before exiting the program */
	if (ret & add_sub_BADNAME) {
		invalid_bundle_provided = true;
	}

	/* Set the version of the subscribed bundles to the one they last changed */
	set_subscription_versions(mom, NULL, subs);

	/* Load the manifest of all bundles to be installed */
	to_install_bundles = recurse_manifest(mom, *subs, NULL, false, NULL);
	if (!to_install_bundles) {
		error("Cannot load to install bundles\n");
		ret = SWUPD_RECURSE_MANIFEST;
		goto out;
	}

	/* Load the manifest of all bundles already installed */
	read_subscriptions(&current_subs);
	set_subscription_versions(mom, NULL, &current_subs);
	installed_bundles = recurse_manifest(mom, current_subs, NULL, false, NULL);
	if (!installed_bundles) {
		error("Cannot load installed bundles\n");
		ret = SWUPD_RECURSE_MANIFEST;
		goto out;
	}
	mom->submanifests = installed_bundles;

	progress_complete_step();
	timelist_timer_stop(globals.global_times); // closing: Add bundles and recurse

	/* Step 2: Get a list with all files needed to be installed for the requested bundles */
	timelist_timer_start(globals.global_times, "Consolidate files from bundles");
	progress_set_step(2, "consolidate_files");

	/* get all files already installed in the target system */
	installed_files = consolidate_files_from_bundles(installed_bundles);
	mom->files = installed_files;
	installed_files = filter_out_deleted_files(installed_files);

	/* get all the files included in the bundles to be added */
	to_install_files = consolidate_files_from_bundles(to_install_bundles);
	to_install_files = filter_out_deleted_files(to_install_files);

	/* from the list of files to be installed, remove those files already in the target system */
	to_install_files = filter_out_existing_files(to_install_files, installed_files);

	progress_complete_step();
	timelist_timer_stop(globals.global_times); // closing: Consolidate files from bundles

	/* Step 3: Check if we have enough space */
	progress_set_step(3, "check_disk_space_availability");
	if (!globals.skip_diskspace_check) {
		timelist_timer_start(globals.global_times, "Check disk space availability");
		char *filepath = NULL;

		bundle_size = get_manifest_list_contentsize(to_install_bundles);
		filepath = mk_full_filename(globals.path_prefix, "/usr/");

		/* Calculate free space on filepath */
		fs_free = get_available_space(filepath);
		free_string(&filepath);

		/* Add 10% to bundle_size as a 'fudge factor' */
		if (((bundle_size * 1.1) > fs_free) || fs_free < 0) {
			ret = SWUPD_DISK_SPACE_ERROR;

			if (fs_free > 0) {
				error("Bundle too large by %ldM\n", (bundle_size - fs_free) / 1000 / 1000);
			} else {
				error("Unable to determine free space on filesystem\n");
			}

			info("NOTE: currently, swupd only checks /usr/ "
			     "(or the passed-in path with /usr/ appended) for available space\n");
			info("To skip this error and install anyways, "
			     "add the --skip-diskspace-check flag to your command\n");

			goto out;
		}
		timelist_timer_stop(globals.global_times); // closing: Check disk space availability
	}
	progress_complete_step();

	/* step 4: download necessary packs */
	timelist_timer_start(globals.global_times, "Download packs");
	progress_set_step(4, "download_packs");

	(void)rm_staging_dir_contents("download");

	if (list_longer_than(to_install_files, 10)) {
		download_subscribed_packs(*subs, mom, true);
	} else {
		/* the progress would be completed within the
		 * download_subscribed_packs function, since we
		 * didn't run it, manually mark the step as completed */
		info("No packs need to be downloaded\n");
		progress_complete_step();
	}
	timelist_timer_stop(globals.global_times); // closing: Download packs

	/* step 5: Download missing files */
	timelist_timer_start(globals.global_times, "Download missing files");
	progress_set_step(5, "download_fullfiles");
	ret = download_fullfiles(to_install_files, NULL);
	if (ret) {
		/* make sure the return code is positive */
		ret = abs(ret);
		error("Could not download some files from bundles, aborting bundle installation\n");
		goto out;
	}
	timelist_timer_stop(globals.global_times); // closing: Download missing files

	/* step 6: Install all bundle(s) files into the fs */
	timelist_timer_start(globals.global_times, "Installing bundle(s) files onto filesystem");
	progress_set_step(6, "install_files");

	info("Installing bundle(s) files...\n");

	/* This loop does an initial check to verify the hash of every downloaded file to install,
	 * if the hash is wrong it is removed from staging area so it can be re-downloaded */
	char *hashpath;
	iter = list_head(to_install_files);
	while (iter) {
		file = iter->data;
		iter = iter->next;

		string_or_die(&hashpath, "%s/staged/%s", globals.state_dir, file->hash);

		if (access(hashpath, F_OK) < 0) {
			/* the file does not exist in the staged directory, it will need
			 * to be downloaded again */
			free_string(&hashpath);
			continue;
		}

		/* make sure the file is not corrupt */
		if (!verify_file(file, hashpath)) {
			warn("hash check failed for %s\n", file->filename);
			info("         will attempt to download fullfile for %s\n", file->filename);
			ret = swupd_rm(hashpath);
			if (ret) {
				error("could not remove bad file %s\n", hashpath);
				ret = SWUPD_COULDNT_REMOVE_FILE;
				free_string(&hashpath);
				goto out;
			}
			// successfully removed, continue and check the next file
			free_string(&hashpath);
			continue;
		}
		free_string(&hashpath);
	}

	/*
	 * NOTE: The following two loops are used to install the files in the target system:
	 *  - the first loop stages the file
	 *  - the second loop renames the files to their final name in the target system
	 *
	 * This process is done in two separate loops to reduce the chance of end up
	 * with a corrupt system if for some reason the process is aborted during this stage
	 */
	unsigned int list_length = list_len(to_install_files) * 2; // we are using two loops so length is times 2
	unsigned int complete = 0;

	/* Copy files to their final destination */
	iter = list_head(to_install_files);
	while (iter) {
		file = iter->data;
		iter = iter->next;
		complete++;

		if (file->is_deleted || file->do_not_update || ignore(file)) {
			continue;
		}

		/* apply the heuristics for the file so the correct post-actions can
		 * be completed */
		apply_heuristics(file);

		/* stage the file:
		 *  - make sure the directory where the file will be copied to exists
		 *  - if the file being staged already exists in the system make sure its
		 *    type hasn't changed, if it has, remove it so it can be replaced
		 *  - copy the file/directory to its final destination; if it is a file,
		 *    keep its name with a .update prefix, if it is a directory, it will already
		 *    be copied with its final name
		 * Note: to avoid too much recursion, do not send the mom to do_staging so it
		 *       doesn't try to fix failures, we will handle those below */
		ret = do_staging(file, mom);
		if (ret) {
			goto out;
		}

		progress_report(complete, list_length);
	}

	/* Rename the files to their final form */
	iter = list_head(to_install_files);
	while (iter) {
		file = iter->data;
		iter = iter->next;
		complete++;

		if (file->is_deleted || file->do_not_update || ignore(file)) {
			continue;
		}

		/* This was staged by verify_fix_path */
		if (!file->staging && !file->is_dir) {
			/* the current file struct doesn't have the name of the "staging" file
			 * since it was staged by verify_fix_path, the staged file is in the
			 * file struct in the MoM, so we need to load that one instead
			 * so rename_staged_file_to_final works properly */
			file = search_file_in_manifest(mom, file->filename);
		}

		rename_staged_file_to_final(file);

		progress_report(complete, list_length);
	}
	sync();
	timelist_timer_stop(globals.global_times); // closing: Installing bundle(s) files onto filesystem

	/* step 7: Run any scripts that are needed to complete update */
	timelist_timer_start(globals.global_times, "Run Scripts");
	progress_set_step(7, "run_scripts");
	scripts_run_post_update(globals.wait_for_scripts);
	timelist_timer_stop(globals.global_times); // closing: Run Scripts
	progress_complete_step();

	ret = SWUPD_OK;

out:
	/* count how many of the requested bundles were actually installed, note that the
	 * to_install_bundles list could also have extra dependencies */
	iter = list_head(to_install_bundles);
	while (iter) {
		struct manifest *to_install_manifest;
		to_install_manifest = iter->data;
		iter = iter->next;
		if (is_installed_bundle(to_install_manifest->component)) {
			if (string_in_list(to_install_manifest->component, bundles)) {
				bundles_installed++;
				track_installed(to_install_manifest->component);
			} else {
				dependencies_installed++;
			}
		}
	}

	/* print totals */
	if (ret && bundles_installed != 0) {
		/* if this point is reached with a nonzero return code and bundles_installed=0 it means that
		* while trying to install the bundles some error occurred which caused the whole installation
		* process to be aborted, so none of the bundles got installed. */
		bundles_failed = bundles_requested - already_installed;
	} else {
		bundles_failed = bundles_requested - bundles_installed - already_installed;
	}

	if (bundles_failed > 0) {
		print("Failed to install %i of %i bundles\n", bundles_failed, bundles_requested - already_installed);
	} else if (bundles_installed > 0) {
		print("Successfully installed %i bundle%s\n", bundles_installed, (bundles_installed > 1 ? "s" : ""));
	}
	if (dependencies_installed > 0) {
		print("%i bundle%s\n", dependencies_installed, (dependencies_installed > 1 ? "s were installed as dependencies" : " was installed as dependency"));
	}
	if (already_installed > 0) {
		print("%i bundle%s already installed\n", already_installed, (already_installed > 1 ? "s were" : " was"));
	}

	/* cleanup */
	if (current_subs) {
		free_subscriptions(&current_subs);
	}
	if (to_install_files) {
		list_free_list(to_install_files);
	}
	if (to_install_bundles) {
		list_free_list_and_data(to_install_bundles, free_manifest_data);
	}
	/* if one or more of the requested bundles was invalid, and
	 * there is no other error return SWUPD_INVALID_BUNDLE */
	if (invalid_bundle_provided && !ret) {
		ret = SWUPD_INVALID_BUNDLE;
	}
	return ret;
}

/* Bundle install one ore more bundles passed in bundles
 * param as a null terminated array of strings
 */
enum swupd_code install_bundles_frontend(char **bundles)
{
	int ret = 0;
	int current_version;
	struct list *aliases = NULL;
	struct list *bundles_list = NULL;
	struct manifest *mom;
	struct list *subs = NULL;
	char *bundles_list_str = NULL;
	bool mix_exists;

	/* initialize swupd and get current version from OS */
	ret = swupd_init(SWUPD_ALL);
	if (ret != 0) {
		error("Failed updater initialization, exiting now\n");
		return ret;
	}

	timelist_timer_start(globals.global_times, "Load MoM");
	current_version = get_current_version(globals.path_prefix);
	if (current_version < 0) {
		error("Unable to determine current OS version\n");
		ret = SWUPD_CURRENT_VERSION_UNKNOWN;
		goto clean_and_exit;
	}

	mix_exists = (check_mix_exists() & system_on_mix());

	mom = load_mom(current_version, mix_exists, NULL);
	if (!mom) {
		error("Cannot load official manifest MoM for version %i\n", current_version);
		ret = SWUPD_COULDNT_LOAD_MOM;
		goto clean_and_exit;
	}
	timelist_timer_stop(globals.global_times); // closing: Load MoM

	timelist_timer_start(globals.global_times, "Prepend bundles to list");
	aliases = get_alias_definitions();
	for (; *bundles; ++bundles) {
		struct list *alias_bundles = get_alias_bundles(aliases, *bundles);
		char *alias_list_str = string_join(", ", alias_bundles);

		if (strcmp(*bundles, alias_list_str) != 0) {
			info("Alias %s will install bundle(s): %s\n", *bundles, alias_list_str);
		}
		free_string(&alias_list_str);
		bundles_list = list_concat(alias_bundles, bundles_list);
	}
	list_free_list_and_data(aliases, free_alias_lookup);
	timelist_timer_stop(globals.global_times); // closing: Prepend bundles to list

	timelist_timer_start(globals.global_times, "Install bundles");
	ret = install_bundles(bundles_list, &subs, mom);
	timelist_timer_stop(globals.global_times); // closing: Install bundles

	timelist_print_stats(globals.global_times);

	free_manifest(mom);
clean_and_exit:
	bundles_list_str = string_join(", ", bundles_list);
	telemetry(ret ? TELEMETRY_CRIT : TELEMETRY_INFO,
		  "bundleadd",
		  "bundles=%s\n"
		  "current_version=%d\n"
		  "result=%d\n"
		  "bytes=%ld\n",
		  bundles_list_str,
		  current_version,
		  ret,
		  total_curl_sz);

	list_free_list_and_data(bundles_list, free);
	free_string(&bundles_list_str);
	free_subscriptions(&subs);
	swupd_deinit();

	return ret;
}

/*
 * This function will read the BUNDLES_DIR (by default
 * /usr/share/clear/bundles/), get the list of local bundles and print
 * them sorted.
 */
enum swupd_code list_local_bundles()
{
	char *name;
	char *path = NULL;
	struct list *bundles = NULL;
	struct list *item = NULL;
	struct manifest *MoM = NULL;
	struct file *bundle_manifest = NULL;
	int current_version;
	bool mix_exists;

	current_version = get_current_version(globals.path_prefix);
	if (current_version < 0) {
		goto skip_mom;
	}

	mix_exists = (check_mix_exists() & system_on_mix());
	MoM = load_mom(current_version, mix_exists, NULL);
	if (!MoM) {
		warn("Could not determine which installed bundles are experimental\n");
	}

skip_mom:
	string_or_die(&path, "%s/%s", globals.path_prefix, BUNDLES_DIR);

	errno = 0;
	bundles = get_dir_files_sorted(path);
	if (!bundles && errno) {
		error("couldn't open bundles directory");
		free_string(&path);
		return SWUPD_COULDNT_LIST_DIR;
	}

	item = bundles;

	while (item) {
		if (MoM) {
			bundle_manifest = search_bundle_in_manifest(MoM, basename((char *)item->data));
		}
		if (bundle_manifest) {
			name = get_printable_bundle_name(bundle_manifest->filename, bundle_manifest->is_experimental);
		} else {
			string_or_die(&name, basename((char *)item->data));
		}
		print("%s\n", name);
		free_string(&name);
		free(item->data);
		item = item->next;
	}

	list_free_list(bundles);

	free_string(&path);
	free_manifest(MoM);

	return SWUPD_OK;
}
