/*
  This file is part of the Astrometry.net suite.
  Copyright 2009 Dustin Lang.

  The Astrometry.net suite is free software; you can redistribute
  it and/or modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation, version 2.

  The Astrometry.net suite is distributed in the hope that it will be
  useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with the Astrometry.net suite ; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
*/

#include <stdint.h>

#include "uniformize-catalog.h"
#include "intmap.h"
#include "healpix.h"
#include "healpix-utils.h"
#include "permutedsort.h"
#include "starutil.h"
#include "an-bool.h"
#include "mathutil.h"
#include "errors.h"
#include "log.h"

struct oh_token {
	//int basehp, x, y;
	int hp;
	int nside;
	int finenside;
};

// Return 1 if the given "hp" is outside the healpix described in "oh_token".
static int outside_healpix(int hp, void* vtoken) {
	struct oh_token* token = vtoken;
	int bighp;
	/*
	 int bhp, bx, by;
	 healpix_decompose_xy(hp, &bhp, &bx, &by, token->finenside);
	 if (bhp != token->basehp)
	 return 1;
	 healpix_convert_xy_nside(bx, by, token->finenside, token->nside, &bx, &by);
	 if (bx == token->x && by == token->y)
	 return 0;
	 return 1;
	 */
	healpix_convert_nside(hp, token->finenside, token->nside, &bighp);
	return (bighp == token->hp ? 0 : 1);
}

static bool is_duplicate(int hp, double ra, double dec, int Nside,
						 intmap_t* starlists,
						 double* ras, double* decs, double dedupr2) {
	double xyz[3];
	int neigh[9];
	int nn;
	double xyz2[3];
	int j, k;
	radecdeg2xyzarr(ra, dec, xyz);
	// Check this healpix...
	neigh[0] = hp;
	// Check neighbouring healpixes... (+1 is to skip over this hp)
	nn = 1 + healpix_get_neighbours(hp, neigh+1, Nside);
	for (k=0; k<nn; k++) {
		int otherhp = neigh[k];
		bl* lst = intmap_find(starlists, otherhp, FALSE);
		if (!lst)
			continue;
		for (j=0; j<bl_size(lst); j++) {
			int otherindex;
			bl_get(lst, j, &otherindex);
			radecdeg2xyzarr(ras[otherindex], decs[otherindex], xyz2);
			if (!distsq_exceeds(xyz, xyz2, 3, dedupr2))
				return TRUE;
		}
	}
	return FALSE;
}

int uniformize_catalog(fitstable_t* intable, fitstable_t* outtable,
					   const char* racol, const char* deccol,
					   const char* sortcol, bool sort_ascending,
					   // ?  Or do this cut in a separate process?
					   int bighp, int bignside,
					   int nmargin,
					   // uniformization nside.
					   int Nside,
					   double dedup_radius,
					   int nsweeps) {
	bool allsky;
	intmap_t* starlists;
	int NHP;
	bool dense = FALSE;
	double dedupr2 = 0.0;
	tfits_type dubl;
	int N;
	int* inorder = NULL;
	int* outorder = NULL;
	int outi;
	double *ra = NULL, *dec = NULL;
	il* myhps = NULL;
	int i,j,k;
	int nkeep = nsweeps;
	int noob = 0;
	int ndup = 0;

    if (Nside % bignside) {
        ERROR("Fine healpixelization Nside must be a multiple of the coarse healpixelization Nside");
        return -1;
    }
	if (Nside > HP_MAX_INT_NSIDE) {
		ERROR("Error: maximum healpix Nside = %i", HP_MAX_INT_NSIDE);
		return -1;
	}

	NHP = 12 * Nside * Nside;
	logverb("Healpix Nside: %i, # healpixes: %i\n", Nside, NHP);
	logverb("Healpix side length: %g arcmin.\n", healpix_side_length_arcmin(Nside));

	dubl = fitscolumn_double_type();
	if (!racol)
		racol = "RA";
	ra = fitstable_read_column(intable, racol, dubl);
	if (!ra) {
		ERROR("Failed to find RA column (%s) in table", racol);
		return -1;
	}
	if (!deccol)
		deccol = "DEC";
	dec = fitstable_read_column(intable, deccol, dubl);
	if (!dec) {
		ERROR("Failed to find DEC column (%s) in table", deccol);
		return -1;
	}

	N = fitstable_nrows(intable);

	// FIXME -- argsort and seek around the input table, and append to
	// starlists in order; OR read from the input table in sequence and
	// sort in the starlists?
	if (sortcol) {
		double *sortval;
		logverb("Sorting by %s...\n", sortcol);
		sortval = fitstable_read_column(intable, sortcol, dubl);
		inorder = permuted_sort(sortval, sizeof(double),
								sort_ascending ? compare_doubles_asc : compare_doubles_desc,
								NULL, N);
		free(sortval);
	}

	allsky = (bighp == -1);
	if (allsky)
        bignside = 0;

	if (!allsky && nmargin) {
		int bigbighp, bighpx, bighpy;
		int ninside;
		il* seeds = il_new(256);
		struct oh_token token;
		logverb("Finding healpixes in range...\n");
        healpix_decompose_xy(bighp, &bigbighp, &bighpx, &bighpy, bignside);
		ninside = (Nside/bignside)*(Nside/bignside);
		// Prime the queue with the fine healpixes that are on the
		// boundary of the big healpix.
		// -1 prevents us from double-adding the corners.
		for (i=0; i<((Nside / bignside) - 1); i++) {
			// add (i,0), (i,max), (0,i), and (0,max) healpixes
            int xx = i + bighpx * (Nside / bignside);
            int yy = i + bighpy * (Nside / bignside);
            int y0 =     bighpy * (Nside / bignside);
            int y1 =(1 + bighpy)* (Nside / bignside) - 1;
            int x0 =     bighpx * (Nside / bignside);
            int x1 =(1 + bighpx)* (Nside / bignside) - 1;
            assert(xx < Nside);
            assert(yy < Nside);
            assert(x0 < Nside);
            assert(x1 < Nside);
            assert(y0 < Nside);
            assert(y1 < Nside);
			il_append(seeds, healpix_compose_xy(bigbighp, xx, y0, Nside));
			il_append(seeds, healpix_compose_xy(bigbighp, xx, y1, Nside));
			il_append(seeds, healpix_compose_xy(bigbighp, x0, yy, Nside));
			il_append(seeds, healpix_compose_xy(bigbighp, x1, yy, Nside));
		}
        logmsg("Number of boundary healpixes: %i (Nside/bignside = %i)\n", il_size(seeds), Nside/bignside);

		token.nside = bignside;
		token.finenside = Nside;
		/*
		 token.basehp = bigbighp;
		 token.x = bighpx;
		 token.y = bighpy;
		 */
		token.hp = bighp;
		myhps = healpix_region_search(-1, seeds, Nside, NULL, NULL,
									  outside_healpix, &token, nmargin);
		il_free(seeds);
	}

	if (myhps)
		il_sort(myhps, TRUE);

	// DEBUG
	il_check_consistency(myhps);
	il_check_sorted_ascending(myhps, TRUE);

	dedupr2 = arcsec2distsq(dedup_radius);
	starlists = intmap_new(sizeof(int32_t), nkeep, 0, dense);

	logverb("Placing stars in grid cells...\n");
	for (i=0; i<N; i++) {
		int hp;
		struct oh_token token;
		bl* lst;
		int32_t j32;

		token.hp = bighp;
		token.nside = bignside;
		token.finenside = Nside;
		if (inorder)
			j = inorder[i];
		else
			j = i;
		
		hp = radecdegtohealpix(ra[j], dec[j], Nside);
		// in bounds?
		if (myhps) {
			if (outside_healpix(hp, &token) ||
				!il_sorted_contains(myhps, hp)) {
				noob++;
				continue;
			}
		} else if (!allsky) {
			if (outside_healpix(hp, &token)) {
				noob++;
				continue;
			}
		}

		lst = intmap_find(starlists, hp, TRUE);
		// is this list full?
		if (nkeep && (bl_size(lst) >= nkeep))
			// Here we assume we're working in sorted order: once the list is full we're done.
			continue;

		if ((dedupr2 > 0.0) &&
			is_duplicate(hp, ra[j], dec[j], Nside, starlists, ra, dec, dedupr2)) {
			ndup++;
			continue;
		}

		// Add the new star (by index)
		j32 = j;
		bl_append(lst, &j32);
	}
	logverb("%i outside the healpix\n", noob);
	logverb("%i duplicates\n", ndup);

	il_free(myhps);
	myhps = NULL;
	free(inorder);
	inorder = NULL;
	free(ra);
	ra = NULL;
	free(dec);
	dec = NULL;

	outorder = malloc(N * sizeof(int));
	outi = 0;

	for (k=0; k<nsweeps; k++) {
		int starti = outi;
		for (i=0;; i++) {
			bl* lst;
			int hp;
			if (!intmap_get_entry(starlists, i, &hp, &lst))
				break;
			if (bl_size(lst) <= k)
				continue;
			bl_get(lst, k, &j);
			outorder[outi] = j;
			outi++;
		}
		logmsg("Sweep %i: %i stars\n", k+1, outi - starti);
	}
	intmap_free(starlists);
	starlists = NULL;

	logmsg("Total: %i stars\n", outi);
	N = outi;

	// Write output.
	/*
	{
		int R;
		char* buf;
		fitstable_add_fits_columns_as_struct(intable);
		fitstable_copy_columns(intable, outtable);
		fitstable_read_extension(intable, 1);
		if (fitstable_write_header(outtable)) {
			ERROR("Failed to write output table header");
			return -1;
		}
		R = fitstable_row_size(intable);
		logmsg("Writing output...\n");
		logverb("Row size: %i\n", R);
		buf = malloc(R);
		for (i=0; i<N; i++) {
			if (fitstable_read_struct(intable, outorder[i], buf)) {
				ERROR("Failed to read data from input table");
				return -1;
			}
			if (fitstable_write_struct(outtable, buf)) {
				ERROR("Failed to write data to output table");
				return -1;
			}
		}
		if (fitstable_fix_header(outtable)) {
			ERROR("Failed to fix output table header");
			return -1;
		}
	 }*/
	{
		int R;
		char* buf;
		fitstable_add_fits_columns_as_struct(intable);
		fitstable_copy_columns(intable, outtable);
		if (fitstable_write_header(outtable)) {
			ERROR("Failed to write output table header");
			return -1;
		}
		R = fitstable_row_size(intable);
		logmsg("Writing output...\n");
		logverb("Row size: %i\n", R);
		buf = malloc(R);
		for (i=0; i<N; i++) {
			if (fitstable_read_row_data(intable, outorder[i], buf)) {
				ERROR("Failed to read data from input table");
				return -1;
			}
			if (fitstable_write_row_data(outtable, buf)) {
				ERROR("Failed to write data to output table");
				return -1;
			}
		}
		if (fitstable_fix_header(outtable)) {
			ERROR("Failed to fix output table header");
			return -1;
		}
	}

	free(outorder);
	return 0;
}
