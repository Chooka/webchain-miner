/**
 * Implementation of the Lyra2 Password Hashing Scheme (PHS).
 *
 * Author: The Lyra PHC team (http://www.lyra-kdf.net/) -- 2014.
 *
 * This software is hereby placed in the public domain.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ''AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Lyra2.h"
#include "Sponge.h"

/**
 * Executes Lyra2 based on the G function from Blake2b. This version supports passwords
 * whose combined length is smaller than the size of the memory matrix, (i.e., (nRows x nCols x b) bits,
 * where "b" is the underlying sponge's bitrate). In this implementation, the "basil" is composed by all
 * integer parameters (treated as type "unsigned int") in the order they are provided, plus the value
 * of nCols, (i.e., basil = kLen || pwdlen || saltlen || timeCost || nRows || nCols).
 *
 * @param K The derived key to be output by the algorithm
 * @param kLen Desired key length
 * @param pwd User password
 * @param pwdlen Password length
 * @param salt Salt
 * @param timeCost Parameter to determine the processing time (T)
 * @param nRows Number or rows of the memory matrix (R)
 * @param nCols Number of columns of the memory matrix (C)
 *
 * @return 0 if the key is generated correctly; -1 if there is an error (usually due to lack of memory for allocation)
 */
int LYRA2(void *ctx2, void *K, int64_t kLen, const void *pwd, int32_t pwdlen)
{
	struct LYRA2_ctx *ctx = ctx2;
	//============================= Basic variables ============================//
	int64_t row = 2; //index of row to be processed
	int64_t prev = 1; //index of prev (last row ever computed/modified)
	int64_t rowa = 0; //index of row* (a previous row, deterministically picked during Setup and randomly picked while Wandering)
	int64_t tau; //Time Loop iterator
	int64_t step = 1; //Visitation step (used during Setup and Wandering phases)
	int64_t window = 2; //Visitation window (used to define which rows can be revisited during Setup)
	int64_t gap = 1; //Modifier to the step, assuming the values 1 or -1
	int64_t i; //auxiliary iteration counter
	int64_t v64; // 64bit var for memcpy
	//==========================================================================/

	//========== Initializing the Memory Matrix and pointers to it =============//
	//Tries to allocate enough space for the whole memory matrix

	const int64_t ROW_LEN_INT64 = BLOCK_LEN_INT64 * NCOLS;
	const int64_t ROW_LEN_BYTES = ROW_LEN_INT64 * 8;
	// for Lyra2REv2, nCols = 4, v1 was using 8
	const int64_t BLOCK_LEN = (NCOLS == 4) ? BLOCK_LEN_BLAKE2_SAFE_INT64 : BLOCK_LEN_BLAKE2_SAFE_BYTES;

	size_t sz = (size_t)ROW_LEN_BYTES * NROWS;
	memset(ctx->wholeMatrix, 0, sz);

	//Places the pointers in the correct positions
	uint64_t *ptrWord = ctx->wholeMatrix;
	for (i = 0; i < NROWS; i++) {
		ctx->memMatrix[i] = ptrWord;
		ptrWord += ROW_LEN_INT64;
	}
	//==========================================================================/

	//============= Getting the password + basil padded with 10*1 ===============//
	//OBS.:The memory matrix will temporarily hold the password: not for saving memory,
	//but this ensures that the password copied locally will be overwritten as soon as possible

	//First, we clean enough blocks for the password, basil and padding
	int64_t nBlocksInput = ((pwdlen + 6 * sizeof(uint64_t)) / BLOCK_LEN_BLAKE2_SAFE_BYTES) + 1;

	byte *ptrByte = (byte*) ctx->wholeMatrix;

	//Prepends the password
	memcpy(ptrByte, pwd, pwdlen);
	ptrByte += pwdlen;
	memset(ptrByte, 0, (size_t) (nBlocksInput * BLOCK_LEN_BLAKE2_SAFE_BYTES - pwdlen));

	//Concatenates the basil: every integer passed as parameter, in the order they are provided by the interface
	memcpy(ptrByte, &kLen, sizeof(int64_t));
	ptrByte += sizeof(uint64_t);
	v64 = pwdlen;
	memcpy(ptrByte, &v64, sizeof(int64_t));
	ptrByte += sizeof(uint64_t);
	v64 = 0; // saltlen
	memcpy(ptrByte, &v64, sizeof(int64_t));
	ptrByte += sizeof(uint64_t);
	v64 = TCOST;
	memcpy(ptrByte, &v64, sizeof(int64_t));
	ptrByte += sizeof(uint64_t);
	v64 = NROWS;
	memcpy(ptrByte, &v64, sizeof(int64_t));
	ptrByte += sizeof(uint64_t);
	v64 = NCOLS;
	memcpy(ptrByte, &v64, sizeof(int64_t));
	ptrByte += sizeof(uint64_t);

	//Now comes the padding
	*ptrByte = 0x80; //first byte of padding: right after the password
	ptrByte = (byte*) ctx->wholeMatrix; //resets the pointer to the start of the memory matrix
	ptrByte += nBlocksInput * BLOCK_LEN_BLAKE2_SAFE_BYTES - 1; //sets the pointer to the correct position: end of incomplete block
	*ptrByte ^= 0x01; //last byte of padding: at the end of the last incomplete block
	//==========================================================================/

	//======================= Initializing the Sponge State ====================//
	//Sponge state: 16 uint64_t, BLOCK_LEN_INT64 words of them for the bitrate (b) and the remainder for the capacity (c)
	uint64_t state[16];
	initState(state);
	//==========================================================================/

	//================================ Setup Phase =============================//
	//Absorbing salt, password and basil: this is the only place in which the block length is hard-coded to 512 bits
	ptrWord = ctx->wholeMatrix;
	for (i = 0; i < nBlocksInput; i++) {
		absorbBlockBlake2Safe(state, ptrWord); //absorbs each block of pad(pwd || salt || basil)
		ptrWord += BLOCK_LEN; //goes to next block of pad(pwd || salt || basil)
	}

	//Initializes M[0] and M[1]
	reducedSqueezeRow0(state, ctx->memMatrix[0]); //The locally copied password is most likely overwritten here

	reducedDuplexRow1(state, ctx->memMatrix[0], ctx->memMatrix[1]);

	do {
		//M[row] = rand; //M[row*] = M[row*] XOR rotW(rand)

		reducedDuplexRowSetup(state, ctx->memMatrix[prev], ctx->memMatrix[rowa], ctx->memMatrix[row]);

		//updates the value of row* (deterministically picked during Setup))
		rowa = (rowa + step) & (window - 1);
		//update prev: it now points to the last row ever computed
		prev = row;
		//updates row: goes to the next row to be computed
		row++;

		//Checks if all rows in the window where visited.
		if (rowa == 0) {
		step = window + gap; //changes the step: approximately doubles its value
		window *= 2; //doubles the size of the re-visitation window
		gap = -gap; //inverts the modifier to the step
	}

	} while (row < NROWS);
	//==========================================================================/

	//============================ Wandering Phase =============================//
	row = 0; //Resets the visitation to the first row of the memory matrix
	for (tau = 1; tau <= TCOST; tau++) {
		//Step is approximately half the number of all rows of the memory matrix for an odd tau; otherwise, it is -1
		step = (tau % 2 == 0) ? -1 : NROWS / 2 - 1;
		do {
			//Selects a pseudorandom index row*
			//------------------------------------------------------------------------------------------
			rowa = state[0] & (unsigned int)(NROWS-1);  //(USE THIS IF NROWS IS A POWER OF 2)
			//rowa = state[0] % NROWS; //(USE THIS FOR THE "GENERIC" CASE)
			//------------------------------------------------------------------------------------------
			__builtin_prefetch((uint64_t*)(ctx->memMatrix[rowa])+0);
			__builtin_prefetch((uint64_t*)(ctx->memMatrix[rowa])+4);
			__builtin_prefetch((uint64_t*)(ctx->memMatrix[rowa])+8);

			//Performs a reduced-round duplexing operation over M[row*] XOR M[prev], updating both M[row*] and M[row]
			reducedDuplexRow(state, ctx->memMatrix[prev], ctx->memMatrix[rowa], ctx->memMatrix[row], ctx);

			//update prev: it now points to the last row ever computed
			prev = row;

			//updates row: goes to the next row to be computed
			//------------------------------------------------------------------------------------------
			row = (row + step) & (unsigned int)(NROWS-1); //(USE THIS IF NROWS IS A POWER OF 2)
			//row = (row + step) % NROWS; //(USE THIS FOR THE "GENERIC" CASE)
			//------------------------------------------------------------------------------------------
			int64_t nrow = (row + step) & (unsigned int)(NROWS-1); //(USE THIS IF NROWS IS A POWER OF 2)
			__builtin_prefetch((uint64_t*)(ctx->memMatrix[nrow])+0);
			__builtin_prefetch((uint64_t*)(ctx->memMatrix[nrow])+4);
			__builtin_prefetch((uint64_t*)(ctx->memMatrix[nrow])+8);

		} while (row != 0);
	}

	//============================ Wrap-up Phase ===============================//
	//Absorbs the last block of the memory matrix
	absorbBlock(state, ctx->memMatrix[rowa]);

	//Squeezes the key
	squeeze(state, K, (unsigned int) kLen);

	return 0;
}

void *LYRA2_create(void)
{
	struct LYRA2_ctx *ctx = malloc(sizeof(struct LYRA2_ctx));

	const int64_t ROW_LEN_INT64 = BLOCK_LEN_INT64 * NCOLS;
	const int64_t ROW_LEN_BYTES = ROW_LEN_INT64 * 8;

	size_t sz = (size_t)ROW_LEN_BYTES * NROWS;
	ctx->wholeMatrix = malloc(sz);
	if (ctx->wholeMatrix == NULL) {
		free(ctx);
		return NULL;
	}

	//Allocates pointers to each row of the matrix
	ctx->memMatrix = malloc(sizeof(uint64_t*) * NROWS);
	if (ctx->memMatrix == NULL) {
		free(ctx->wholeMatrix);
		free(ctx);
		return NULL;
	}

	return ctx;
}

void LYRA2_destroy(void *c)
{
	struct LYRA2_ctx *ctx = c;
	if (ctx) {
		if (ctx->wholeMatrix) free(ctx->wholeMatrix);
		if (ctx->memMatrix) free(ctx->memMatrix);
		free(ctx);
	}
}
