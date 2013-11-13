/************************************************************************
 * Copyright (C) 2011-2013:
 *
 *	Edgardo Mejia-Roa(*), Carlos Garcia, Jose Ignacio Gomez,
 *	Manuel Prieto, Francisco Tirado and Alberto Pascual-Montano(**).
 *
 *	(*)  ArTeCS Group, Complutense University of Madrid (UCM), Spain.
 *	(**) Functional Bioinformatics Group, Biocomputing Unit,
 *		National Center for Biotechnology-CSIC, Madrid, Spain.
 *
 *	E-mail for E. Mejia-Roa: <edgardomejia@fis.ucm.es>
 *	E-mail for A. Pascual-Montano: <pascual@cnb.csic.es>
 *
 *
 * This file is part of bioNMF-mGPU..
 *
 * BioNMF-mGPU is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BioNMF-mGPU is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BioNMF-mGPU.  If not, see <http://www.gnu.org/licenses/>.
 *
 ***********************************************************************/
/**********************************************************
 * NMF_GPU.h
 *	Main program for single-GPU version.
 *
 * NOTE: The following macro constants can be defined to modify the
 *	behavior of routines, as well as some constant and data-type definitions.
 *
 *	Additional information:
 *		NMFGPU_VERBOSE: Shows some messages concerning the progress of the program, as well as some configuration parameters.
 *		NMFGPU_VERBOSE_2: Shows the parameters in some routine calls.
 *
 *	Timing:
 *		NMFGPU_PROFILING_GLOBAL: Compute total elapsed time.
 *		NMFGPU_PROFILING_CONV: Compute timing of convergence test. Shows additional information.
 *		NMFGPU_PROFILING_TRANSF: Compute timing of data transfers. Shows additional information.
 *		NMFGPU_PROFILING_KERNELS: Compute timing of CUDA kernels. Shows additional information.
 *
 *	Debug / Testing:
 *		NMFGPU_FIXED_INIT: Initializes matrices W and H with fixed random values.
 *		NMFGPU_DEBUG: Shows the result of each matrix operation and data transfer.
 *		NMFGPU_FORCE_BLOCKS: Forces the processing of the input matrix as four blocks.
 *		NMFGPU_FORCE_DIMENSIONS: Overrides matrix dimensions.
 *		NMFGPU_TEST_BLOCKS: Just shows block information structure. No GPU memory is allocated.
 *
 **********************************************************
 **********************************************************
 **********************************************************
 *
 * Data matrices:
 * 	V (N rows, M columns): Input matrix,
 * 	K: Factorization Rank,
 * 	W (N,K): Output matrix,
 * 	H (K,M): Output matrix,
 * so that V ~ W*H
 *
 * NOTE: In order to improve performance:
 *	- Matrix H is stored in memory as COLUMN-major (i.e., it is transposed).
 *
 *	- All matrices include useless data for padding. Padded dimensions
 *	  are denoted with the 'p' character, e.g., 'Mp' (i.e.,, M + padding)
 *	  or 'Kp' (factorization_rank + padding).
 *
 *	- Padded dimensions are a multiple of memory_alignment
 *	  (a global variable which currently is equal to warpSize or warpSize/2).
 *
 ***************
 *
 * Multi-GPU version:
 *
 * When the input matrix V is distributed among multiple devices each host thread processes
 * the following sets of rows and columns:
 *	Vrow[ 1..NnP ][ 1..M ] <-- V[ bN..(bN+NnP) ][ 1..M ]	(i.e., NnP rows, starting from bN)
 *	Vcol[ 1..N ][ 1..MnP ] <-- V[ 1..N ][ bM..(bM+MnP) ]	(i.e., MnP columns, starting from bM)
 *
 * Such sets allow to update the corresponding rows and columns of W and H, respectively.
 *
 * Note that each host thread has a private copy of matrices W and H, which must be synchronized
 * after being updated.
 *
 ****************
 *
 * Large input matrix (blockwise processing):
 *
 * If the input matrix (or the portion assigned to this device) is too large for the GPU memory,
 * it must be blockwise processed as follow:
 *	d_Vrow[1..BLN][1..Mp] <-- Vrow[ offset..(offset + BLN) ][1..Mp]			(i.e., BLN <= NnP rows)
 *	d_Vcol[1..N][1..BLMp] <-- Vcol[1..N][ offset_Vcol..(offset_Vcol + BLMp) ]	(i.e., BLM <= MnP columns)
 *
 * Note that padded dimensions are denoted with the suffix 'p' (e.g., Mp, BLMp, etc).
 *
 * In any case, matrices W and H are fully loaded into the GPU memory.
 *
 * Information for blockwise processing is stored in two block_t structures (one for each dimension).
 * Such structures ('block_N' and 'block_M') are initialized in init_block_conf() routine.
 *
 *********************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <math.h>	/* sqrt */
#if NMFGPU_PROFILING_GLOBAL
	#include <sys/time.h>
#endif

#include "index_type.h"
#include "real_type.h"
#include "common.h"
#include "matrix/matrix_io_routines.h"
#include "matrix/matrix_io.h"
#if NMFGPU_PROFILING_TRANSF || NMFGPU_PROFILING_KERNELS || NMFGPU_PROFILING_CONV
	#include "timing.cuh"
#endif
#include "GPU_setup.cuh"
#include "matrix/matrix_operations.cuh"
#include "NMF_routines.h"

// ---------------------------------------------
// ---------------------------------------------

// Data Matrices
real *restrict V = NULL;
real *restrict W = NULL;
real *restrict H = NULL;

// Convergence test.
index_t *restrict classification = NULL;
index_t *restrict last_classification = NULL;

// Total time
#if NMFGPU_PROFILING_GLOBAL
	struct timeval tv1,tv2;
	double total_gpu_time = 0;
#endif


/////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////

/*
 * Read matrix from file
 *
 * numeric_hdrs, numeric_lbls: Has <filename> numeric column headers / row headers ?
 * isBinary: Is <filename> a binary file?
 *
 * Returns EXIT_SUCCESS or EXIT_FAILURE
 */
static int init_V( const char *restrict filename, bool numeric_hdrs, bool numeric_lbls, bool isBinary, struct matrix_labels *restrict ml )
{

	#if NMFGPU_VERBOSE
		printf("Initializing input matrix from file %s...\n", filename);
	#endif

	// ----------------------------

	index_t nrows = 0, ncols = 0;

	real *restrict matrix = NULL;

	printf("\nLoading input file...\n");

	if ( is_bin ) {	// Input file is (non-native) binary.

		printf("\tFile selected as (non-\"native\") binary (i.e., double-precision data and unsigned int's). Loading...\n");

		status = matrix_load_binary_verb( filename, &matrix, &nrows, &ncols, &ml );

	} else { // Input file is ASCII-text.

		printf("\nFile selected as ASCII-text. Loading...\n"
			"\t\tData matrix selected as having numeric column headers: %s.\n"
			"\t\tData matrix selected as having numeric row labels: %s.\n",
			( numeric_hdrs ? "Yes" : "No" ), ( numeric_lbls ? "Yes" : "No" ) );

		status = matrix_load_ascii_verb( filename, numeric_hdrs, numeric_lbls, &matrix, &nrows, &ncols, ml );

	} // If file is binary or text.

	if ( status == EXIT_FAILURE ) {
		fprintf( stderr, "Error reading '%s'\n", filename );
		fflush(NULL);
		return EXIT_FAILURE;
	}

	// --------------------------------

	#if NMFGPU_FORCE_DIMENSIONS
		/* Ignores dimensions in file and uses the ones provided.
		 * NOTE: Both N and M are global variables.
		 */
		if ( (N > 0) * (M > 0) ) {	// N>0 && M>0
			if ( N*M <= nrows*ncols ) {
				nrows = N;
				ncols = M;
			} else {
				nrows = MIN_VAL( N, nrows );
				ncols = MIN_VAL( M, ncols );
			}
			printf( "\nForcing to N=%" PRI_IDX ", M=%" PRI_IDX "\n", nrows, ncols );
		}
	#endif

	// --------------------------------

	// Sets global variables.
	NnP = N = nrows;
	MnP = M = ncols;
	MnPp = Mp = get_padding( M );

	// --------------------------------

	// Changes matrix to a PINNED HOST memory.

	V = getHostMemory( N * Mp * sizeof(real), true );	// Write Combined
	if ( ! V ) {
		fprintf( stderr, "Error allocating memory for input matrix,\n" );
		matrix_clean( matrix, ml );
		fflush(NULL);
		return EXIT_FAILURE;
	}


	// Copies input matrix to the new memory
	real *pV = V;
	real *pMatrix = matrix;
	for ( index_t i=0 ; i < N ; i++, pV += Mp, pMatrix += M ) {

		real *p = memcpy( pV, pMatrix, M * sizeof(real) );
		if ( ! p ) {
			int const err = errno; fflush(stdout); errno = err;
			fprintf( stderr, "memcpy:%sError setting memory for input matrix (row %" PRI_IDX " / %" PRI_IDX ").\n",
				( errno ? " " strerror(errno) "\n" : " " ), i, N );
			freeHostMemory( V );
			matrix_clean( matrix, ml );
			fflush(NULL);
			return EXIT_FAILURE;
		}

		for ( index_t j = M ; j < Mp ; j++ )	// Padding
			pV[ j ] = REAL_C( 0.0 );

	} // for

	// --------------------------------

	free( matrix );

	return EXIT_SUCCESS;

} // init_V

/////////////////////////////////////////////////////////////////////

/*
 * NMF algorithm
 *
 * Return EXIT_SUCCESS or EXIT_FAILURE.
 */
int nmf( index_t nIters, index_t niter_test_conv, index_t stop_threshold )
{

	int status

	// Pointers to d_W and d_H
	pd_H = d_H;
	pd_W = d_W;

	pVcol = pVrow = V;

	// ----------------------------

	// Initializes matrices W and H with random values.

	status = init_randomGenerator();
	if ( status == EXIT_FAILURE )
		return EXIT_FAILURE;

	status = set_randomGenerator_seed( 575435 ); // FIXME: seed!
	if ( status == EXIT_FAILURE ) {
		finalize_randomGenerator();
		return EXIT_FAILURE;
	}

	// W
	matrix_random( d_W, N, K,
			#if NMFGPU_DEBUG || NMFGPU_VERBOSE_2
				false, "d_W",
			#endif
			Kp );

	// H
	matrix_random( d_H, M, K,
			#if NMFGPU_DEBUG || NMFGPU_VERBOSE_2
				true, "d_H",
			#endif
			Kp );

	finalize_randomGenerator();

	// ----------------------------

	// Uploads matrix V
	{
		// Block configuration.
		index_t BLM = block_M.BL[ pBLM ];		// Number of columns.
		index_t BLMp = block_M.BLp[ pBLM ];		// Number of columns (with padding).
		index_t BLN = block_N.BL[ pBLN ];		// Number of rows.

		// d_Vcol
		if ( d_Vcol != d_Vrow )
			upload_matrix_partial( pVcol, N, MnPp, offset_Vcol, BLM, "Vcol", "d_Vcol", BLMp, d_Vcol, Vcol_event, Vcol_stream,
						&upload_Vcol_timing );

		// d_Vrow
		upload_matrix_partial( pVrow, BLN, Mp, 0, M, "Vrow", "d_Vrow", Mp, Vrow_stream, Vrow_event, &upload_Vrow_timing );
	}

	// ----------------------------

	// Number of iterations

	div_t niter_div = div( nIters , niter_test_conv );
	index_t niter_conv = (index_t) niter_div.quot;		// Number of times to perform test of convergence.
	index_t niter_rem = (index_t) niter_div.rem;		// Remaining iterations.
	bool converged = false;

	#if NMFGPU_VERBOSE
		printf("niter_test_conv=%" PRI_IDX ", niter_conv=%" PRI_IDX ", niter_rem=%" PRI_IDX "\n",
			niter_test_conv, niter_div.quot, niter_div.rem);
	#endif

	// ------------------------

	index_t inc = 0;	// Number of it. w/o changes.

	/* Performs all <nIters> iterations in <niter_conv> groups
	 * of <niter_test_conv> iterations each.
	 */

	for ( index_t iter=0 ; iter<niter_conv ; iter++ ) {

		// Runs NMF for niter_test_conv iterations...
		for ( index_t i=0 ; i<niter_test_conv ; i++ ) {

			#if NMFGPU_DEBUG
			///////////////////////////////
				printf("\n\n============ iter=%" PRI_IDX ", Loop %" PRI_IDX " (niter_test_conv): ============\n"
					"------------ Matrix H: ------------\n",iter,i);
			/////////////////////////////
			#endif

			// Reduces d_W to a row.
			matrix_to_row( d_W, block_N.BL[ pBLN ], Kp,
					#if NMFGPU_DEBUG_REDUCT || NMFGPU_DEBUG
						K, "d_W",
					#endif
					d_Aux, d_accum );

			/*
			 * WH(N,BLMp) = W * pH(BLM,Kp)
			 * WH(N,BLMp) = Vcol(N,BLMp) ./ WH(N,BLMp)
			 * Haux(BLM,Kp) = W' * WH(N,BLMp)
			 * H(BLM,Kp) = H(BLM,Kp) .* Haux(BLM,Kp) ./ accum_W
			 */
			update_H();

			#ifdef NMFGPU_DEBUG
			///////////////////////////////
			printf("\n------------ iter=%i, loop %i (niter_test_conv) Matrix W: ------------\n",iter,i);
			/////////////////////////////
			#endif

			// Reduces d_H to a row.
			matrix_to_row( d_H, block_M.BL[ pBLM ], Kp,
					#if NMFGPU_DEBUG_REDUCT || NMFGPU_DEBUG
						K, "d_H",
					#endif
					d_Aux, d_accum );

			/*
			 * WH(BLN,Mp) = W(BLN,Kp) * H
			 * WH(BLN,Mp) = Vrow(BLN,Mp) ./ WH(BLN,Mp)
			 * Waux(BLN,Kp) = WH(BLN,Mp) * H'
			 * W(BLN,Kp) = W(BLN,Kp) .* Waux(BLN,Kp) ./ accum_h
			 */
			update_W();

		} // for niter_test_conv times.

		// -------------------------------------

		// Adjusts matrices W and H.

		adjust_matrix( d_H, block_M.BL[ pBLM ],
					#if NMFGPU_DEBUG
						true, "d_H",
					#endif
				matrix_stream, matrix_event );

		adjust_matrix( d_W, block_N.BL[ pBLN ],
					#if NMFGPU_DEBUG
						false, "d_W",
					#endif
				stream_NMF[ psNMF_N ], matrix_event );

		// -------------------------------------

		// Test of convergence

		// Computes classification vector
		get_classification( classification );

		// Computes differences
		index_t diff = get_difference( classification, last_classification, M );

		// -------------------------------------

		// Saves the new classification.
		{
			// It just swaps the pointers.
			int *tmp = classification;
			classification = last_classification;
			last_classification = tmp;
		}

		// Stops if Connectivity matrix (actually, classification vector) has not changed over last <stop_threshold> iterations.
		if ( diff )
			inc = 0;	// Restarts counter.

		// Increments the counter.
		else if ( inc < stop_threshold )
			inc++;

		#if ! NMFGPU_DEBUG
		// Algorithm has converged.
		else {
			iter++; // Adds to counter the last <niter_test_conv> iterations performed
			converged = true;
			break;
		}
		#endif

	} // for  ( nIters / niter_test_conv ) times

	// ---------------------------------------------------------

	// Remaining iterations (if NMF has not converged yet).

	if ( (!converged) * niter_rem ) { // (converged == 0) && (niter_rem > 0)

		#if NMFGPU_VERBOSE
			printf("Performing remaining iterations (%" PRI_IDX ")...\n",niter_rem);
		#endif

		// Runs NMF for niter_rem iterations...
		for ( index_t i=0 ; i<niter_rem ; i++ ) {

			#ifdef _DEBUG_NMF
			///////////////////////////////
			printf("\n\n============ Loop %" PRI_IDX " (remaining) ============\n------------ Matrix H: ------------\n",i);
			/////////////////////////////
			#endif

			// Reduces d_W to a row.
			matrix_to_row( d_W, block_N.BL[ pBLN ], Kp,
					#if NMFGPU_DEBUG_REDUCT || NMFGPU_DEBUG
						K, "d_W",
					#endif
					d_Aux, d_accum );

			/*
			 * WH(N,BLMp) = W * pH(BLM,Kp)
			 * WH(N,BLMp) = Vcol(N,BLMp) ./ WH(N,BLMp)
			 * Haux(BLM,Kp) = W' * WH(N,BLMp)
			 * H(BLM,Kp) = H(BLM,Kp) .* Haux(BLM,Kp) ./ accum_W
			 */
			update_H();

			#ifdef NMFGPU_DEBUG
			///////////////////////////////
			printf("\n------------ Matrix W (loop=%" PRI_IDX ",remaining): ------------\n",i);
			/////////////////////////////
			#endif

			// Reduces d_H to a row.
			matrix_to_row( d_H, block_M.BL[ pBLM ], Kp,
					#if NMFGPU_DEBUG_REDUCT || NMFGPU_DEBUG
						K, "d_H",
					#endif
					d_Aux, d_accum );

			/*
			 * WH(BLN,Mp) = W(BLN,Kp) * H
			 * WH(BLN,Mp) = Vrow(BLN,Mp) ./ WH(BLN,Mp)
			 * Waux(BLN,Kp) = WH(BLN,Mp) * H'
			 * W(BLN,Kp) = W(BLN,Kp) .* Waux(BLN,Kp) ./ accum_h
			 */
			update_W();

		} // for niter_rem times.

	} // if has not yet converged.

	#if NMFGPU_VERBOSE
		printf( "Done" );
	#endif

	// --------------------------------

	// Number of iterations performed.

	index_t num_iter_performed = nIters;
	if ( converged ) {
		num_iter_performed = iter * niter_test_conv;
		printf("\nNMF: Algorithm converged in %" PRI_IDX " iterations.\n\n", num_iter_performed );
	}
	else
		printf("\nNMF: %" PRI_IDX " iterations performed.\n\n", num_iter_performed );

	// --------------------------------

	// Downloads output matrices

	// d_H
	download_matrix( H, M, Kp, d_H,
		#if NMFGPU_DEBUG || NMFGPU_DEBUG_TRANSF || NMFGPU_VERBOSE_2
			M, true, "H", "d_H",
		#endif
		&download_H_timing );

	// d_W
	download_matrix( W, N, Kp, d_W,
		#if NMFGPU_DEBUG || NMFGPU_DEBUG_TRANSF || NMFGPU_VERBOSE_2
			N, false, "W", "d_W",
		#endif
		&download_W_timing );

	// --------------------------------

	if ( check_cuda_status() == EXIT_FAILURE )
		return EXIT_FAILURE;

	return EXIT_SUCCESS;

} // nmf

/////////////////////////////////////////////////////////////////////

/*
 * Writes output matrices
 *
 * Returns EXIT_SUCCESS or EXIT_FAILURE
 */
static int write_matrices( const char *restrict filename, struct matrix_labels ml )
{

	// Initializes labels for output matrices.

	#if NMFGPU_VERBOSE || NMFGPU_DEBUG_NMF
		printf("\tInitializing labels for output matrices...\n");
	#endif

	// There are column headers.
	bool col_headers = ( ( ml.name != NULL ) || ( ml.headers.tokens != NULL ) );

	// There are matrix labels.
	bool is_labeled = col_headers || ( ml.labels.tokens != NULL );

	// Labels for dimension K.
	struct tags labels_factor = NEW_TAGS(  NULL, NULL );

	if ( ( is_labeled ) && ( generate_labels( "Factor_", NULL, 0, K, &labels_factor ) == EXIT_FAILURE ) ) {
		fflush(stdout);
		fprintf(stderr,"Error initalizing temporary data (labels_factor).\n");
		return EXIT_FAILURE;
	}

	struct matrix_labels labels_H = { ml.name, ml.headers, labels_factor };
	struct matrix_labels labels_W = { ml.name, labels_factor, ml.labels };

	// -----------------------------

	char *filename_out = (char *) malloc( (strlen(filename) + 6)+sizeof(char) );	// <filename> + [ '_W.txt' | '_H.txt' ]
	if ( filename_out == NULL ) {
		int err = errno; fflush(stdout); errno = err;
		perror( "\nmalloc(filename_out) " );
		clean_labels( labels_factor );
		return EXIT_FAILURE;
	}

	// Matrix W:
	if ( sprintf( filename_out, "%s_W.txt", filename ) <= 0 ) {
		int err = errno; fflush(stdout); errno = err;
		perror( "\nsprintf(filename_out, W) " );
		free( filename_out );
		clean_labels( labels_factor );
		return EXIT_FAILURE;
	}

	if ( isBinary )
		status = matrix_save_binary( filename_out, W, N, K, false, &labels_W, Kp );
	else
		status = matrix_save_ascii( filename_out, W, N, K, false, false, &labels_W, Kp );

	if ( status == EXIT_FAILURE ) {
		fprintf( stderr, "Error writing matrix W.\n" );
		free( filename_out );
		clean_labels( labels_factor );
		return EXIT_FAILURE;
	}

	// Matrix H:
	if ( sprintf( filename_out, "%s_H.txt", filename ) <= 0 ) {
		int err = errno; fflush(stdout); errno = err;
		perror( "\nsprintf(filename_out, H) " );
		free( filename_out );
		clean_labels( labels_factor );
		return EXIT_FAILURE;
	}

	if ( isBinary )
		status = matrix_save_binary( filename_out, H, K, M, true, &labels_H, Kp );
	else
		status = matrix_save_ascii( filename_out, H, K, M, true, false, &labels_H, Kp );
	if (status == EXIT_FAILURE ) {
		fprintf( stderr, "Error writting matrix H\n" );
		free( filename_out );
		clean_labels( labels_factor );
		return EXIT_FAILURE;
	}
	free( filename_out );
	clean_labels( labels_factor );

	return EXIT_SUCCESS;

} // write_matrices

/////////////////////////////////////////////////////////////////////

/*
 * Main program
 *
 * Returns EXIT_SUCCESS or EXIT_FAILURE.
 */
int main( int argc, char **argv )
{

	int status = EXIT_SUCCESS;

	// ----------------------------------------

	/* Reads all parameters and performs error-checking. */

	bool help = false;	// Help message requested

	struct input_arguments arguments;	// Input arguments

	// Checks all arguments (shows error messages).
	if ( check_arguments( argc, argv, true, &help, &arguments ) == EXIT_FAILURE ) {
		if ( help ) {
			fprintf(stderr, "\n==========\n");
			print_help( *argv, stderr );
		}
		fflush(NULL);
		return EXIT_FAILURE;
	}

	// If help was requested, just prints a help message and returns.
	if ( help ) {
		print_help( *argv, stdout );
		fflush(NULL);
		return EXIT_SUCCESS;
	}

	char const *restrict const filename = arguments.filename;	// Output filename
	bool const is_bin = arguments.is_bin;				// File is (non-"native") binary.
	bool const numeric_hdrs = arguments.numeric_hdrs;		// Has numeric columns headers.
	bool const numeric_lbls = arguments.numeric_lbls;		// Has numeric row labels.
		K = arguments.k;					// Factorization rank.
	index_t const nIters = arguments.nIters;			// Maximum number of iterations per run.
	index_t const niter_test_conv = arguments.niter_test_conv;	// Number of iterations before testing convergence.
	index_t const stop_threshold = arguments.stop_threshold;	// Stopping criterion.
	index_t const gpu_device = arguments.gpu_device;		// Device ID.


	// Perform a convergence test??
	bool const need_conv = ( nIters >= niter_test_conv );


	#if NMFGPU_FORCE_DIMENSIONS
		/* Uses optional arguments to force matrix dimensions.
		 * NOTE: Both N and M are global variables.
		 */
		N = arguments.N;
		M = arguments.M;
	#endif

	#if NMFGPU_DEBUG
		// Removes the buffer associated to 'stdout' in order to prevent losing messages if the program crashes.
		setbuf( stdout, NULL );
	#endif

	// ----------------------------------------

	// Initializes the GPU device

	// Total global memory
	size_t mem_size = 0;

	status = init_GPU( gpu_device, &mem_size );
	if ( status == EXIT_FAILURE ) {
		fflush(NULL);
		return EXIT_FAILURE;
	}

	// Padding for K dimensions
	Kp = get_padding( K );

	// ----------------------------------------

	// Initializes other CUDA structures
	status = init_GPUdevice( 1, mem_size, need_conv );
	if ( status == EXIT_FAILURE ) {
		finalize_GPU();
		fflush(NULL);
		return EXIT_FAILURE;
	}

	// ----------------------------------------

	// Reads input matrix

	struct matrix_labels ml = NEW_MATRIX_LABELS( NULL, NULL, NULL, NULL, NULL );

	status = init_V( filename, numeric_hdrs, numeric_lbls, isBinary, &ml );
	if ( status == EXIT_FAILURE ) {
		finalize_GPUdevice();
		fflush(NULL);
		return EXIT_FAILURE;
	}

	// ----------------------------------------

	// Initializes matrices W and H

	W = getHostMemory( N * Kp * sizeof(real), false );
	if ( status == EXIT_FAILURE ) {
		fprintf( stderr, "Error allocating memory for HOST matrix W (N=%" PRI_IDX ", Kp=%" PRI_IDX ").\n", N, Kp );
		freeHostMemory( V ); clean_matrix_labels( ml );
		finalize_GPUdevice();
		fflush(NULL);
		return EXIT_FAILURE;
	}

	H = getHostMemory( M * Kp * sizeof(real), false );
	if ( status == EXIT_FAILURE ) {
		fprintf( stderr, "Error allocating memory for HOST matrix H (M=%" PRI_IDX ", Kp=%" PRI_IDX ").\n", M, Kp );
		freeHostMemory( W ); freeHostMemory( V ); clean_matrix_labels( ml );
		finalize_GPUdevice();
		fflush(NULL);
		return EXIT_FAILURE;
	}

	// ----------------------------------------

	// Initializes classification vectors.

	if ( need_conv ) {
		classification = getHostMemory( Mp * sizeof(index_t), false );
		if ( status == EXIT_FAILURE ) {
			fprintf( stderr, "Error allocating memory for HOST classification vector (M=%" PRI_IDX ", Mp=%" PRI_IDX ").\n", M, Mp );
			freeHostMemory( H ); freeHostMemory( W ); freeHostMemory( V ); clean_matrix_labels( ml );
			finalize_GPUdevice();
			fflush(NULL);
			return EXIT_FAILURE;
		}

		last_classification = getHostMemory( Mp * sizeof(index_t), false );
		if ( status == EXIT_FAILURE ) {
			fprintf( stderr, "Error allocating memory for HOST classification vector (last, M=%" PRI_IDX ", Mp=%" PRI_IDX ").\n",
				M, Mp );
			freeHostMemory(classification); freeHostMemory( H ); freeHostMemory( W ); freeHostMemory( V ); clean_matrix_labels( ml );
			finalize_GPUdevice();
			fflush(NULL);
			return EXIT_FAILURE;
		}

		// Cleans last_classification.
		real *p = memset( last_classification, 0, Mp * sizeof(index_t) );
		if ( ! p ) {
			fprintf( stderr, "Error initializing memory for HOST classification vector (last, M=%" PRI_IDX ", Mp=%" PRI_IDX ").\n",
				M, Mp );
			freeHostMemory(last_classification); freeHostMemory(classification);
			freeHostMemory( H ); freeHostMemory( W ); freeHostMemory( V ); clean_matrix_labels( ml );
			finalize_GPUdevice();
			fflush(NULL);
			return EXIT_FAILURE;
		}

	} // need_conv

	// ----------------------------------------

	// Initializes time data structures

	#if NMFGPU_PROFILING_TRANSF || NMFGPU_PROFILING_KERNELS || NMFGPU_PROFILING_CONV

		#if NMFGPU_PROFILING_KERNELS
			init_kernel_timers();
		#endif

		#if NMFGPU_PROFILING_TRANSF
			init_transfer_timers();
		#endif

		#if NMFGPU_PROFILING_CONV
			init_conv_timer();
		#endif

		status = init_timing_events( device_id );

		if ( status == EXIT_FAILURE ) {
			if ( need_conv ) { freeHostMemory(last_classification); freeHostMemory(classification); }
			freeHostMemory( H ); freeHostMemory( W ); freeHostMemory( V ); clean_matrix_labels( ml );
			finalize_GPUdevice();
			fflush(NULL);
			return EXIT_FAILURE;
		}

	#endif

	// ----------------------------------------

	// Executes the NMF Algorithm

	status = nmf( nIters, niter_test_conv, stop_threshold );
	if ( status == EXIT_FAILURE ) {
		destroy_timing_events( device_id );
		if ( need_conv ) { freeHostMemory(last_classification); freeHostMemory(classification); }
		freeHostMemory( H ); freeHostMemory( W ); freeHostMemory( V ); clean_matrix_labels( ml );
		finalize_GPUdevice();
		fflush(NULL);
		return EXIT_FAILURE;
	}

	// ----------------------------------------

	/* Checks results:
	 *
	 * Computes the "distance" between V and W*H as follow:
	 *
	 *	distance = norm( V - (W*H) ) / norm( V ),
	 * where
	 *	norm( X ) = sqrt( dot_X )
	 *	dot_V	 <-- dot_product( V, V )
	 *	dot_VWH  <-- dot_product( V-(W*H), V-(W*H) )
	 */

	real dot_V = REAL_C( 0.0 ), dot_VWH = REAL_C( 0.0 );

	status = dot_product_VWH( &dot_V, &dot_VWH );
	if ( status == EXIT_FAILURE ) {
		destroy_timing_events( device_id );
		if ( need_conv ) { freeHostMemory(last_classification); freeHostMemory(classification); }
		freeHostMemory( H ); freeHostMemory( W ); freeHostMemory( V ); clean_matrix_labels( ml );
		finalize_GPUdevice();
		fflush(NULL);
		return EXIT_FAILURE;
	}

	printf( "\nDistance between V and W*H: %g\n", SQRTR( dot_VWH ) / SQRTR( dot_V ) );

	// ----------------------------------------

	// Writes output matrices.

	printf( "\nWriting output matrices.\n" )

	status = write_matrices( filename, ml );
	if ( status == EXIT_FAILURE ) {
		destroy_timing_events( device_id );
		if ( need_conv ) { freeHostMemory(last_classification); freeHostMemory(classification); }
		freeHostMemory(last_classification); freeHostMemory(classification);
		freeHostMemory( H ); freeHostMemory( W ); freeHostMemory( V ); clean_matrix_labels( ml );
		finalize_GPUdevice();
		fflush(NULL);
		return EXIT_FAILURE;
	}

	if ( need_conv ) { freeHostMemory(last_classification); freeHostMemory(classification); }

	// ----------------------------------------

	// Show elapsed time:

	#if NMFGPU_PROFILING_TRANSF || NMFGPU_PROFILING_KERNELS || NMFGPU_PROFILING_CONV

		printf( "\nElapsed time:\n" );

		#if NMFGPU_PROFILING_KERNELS
			show_kernel_times();
		#endif

		#if NMFGPU_PROFILING_TRANSF
			show_transfer_times();
		#endif

		destroy_timing_events( device_id );

	#endif

	// ----------------------------------------

	freeHostMemory( H ); freeHostMemory( W ); freeHostMemory( V ); clean_matrix_labels( ml );

	status = finalize_GPUdevice();

	fflush(NULL);

	return status;

} // main

/////////////////////////////////////////////////////////////////////