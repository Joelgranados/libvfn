// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2025 The libvfn Authors. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <stdio.h>
#include "ccan/compiler/compiler.h"
#include "ccan/opt/opt.h"
#include "ccan/str/str.h"

static char *opt_mmc_bdf = "";
static char *opt_mc_bdf = "";
static bool s_usage = false;
static uint opt_size_entry_nr = 0;

static struct opt_table opts[] = {
	OPT_WITHOUT_ARG("-h|--help", opt_set_bool, &s_usage, "show usage"),
	OPT_WITH_ARG("-mmc-bdf", opt_set_charp, opt_show_charp, &opt_mmc_bdf,
			"Migration Manager Controller B:D:F Id"),
	OPT_WITH_ARG("--mc-bdf", opt_set_charp, opt_show_charp, &opt_mc_bdf,
			"Migratable controller B:D:F Id"),
	OPT_WITH_ARG("--size-entry-nr", opt_set_uintval, opt_show_uintval, &opt_size_entry_nr,
			"Number of 32 byte migration CDQ entries"),
};

int main(int argc, char **argv)
{
	opt_register_table(opts, NULL);
	opt_parse(&argc, argv, opt_log_stderr_exit);

	if (s_usage)
		opt_usage_and_exit(NULL);

	if (streq(opt_mmc_bdf, ""))
		opt_usage_exit_fail("missing --mmc-bdf migration manager controller arg");
	if (streq(opt_mc_bdf, ""))
		opt_usage_exit_fail("missing --mc-bdf migratable controller arg");
	if (opt_size_entry_nr == 0)
		opt_usage_exit_fail("--entry-nr must be > 0");

	opt_free_table();

	fprintf(stderr, "Hello world");
}
