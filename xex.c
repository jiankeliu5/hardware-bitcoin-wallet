/** \file xex.c
  *
  * \brief Implements XEX mode for encryption of a random-access block device.
  *
  * For details, see "Efficient Instantiations of Tweakable Blockciphers and
  * Refinements to Modes OCB and PMAC" (dated September 24, 2004) by Phillip
  * Rogaway, obtained from
  * http://www.cs.ucdavis.edu/~rogaway/papers/offsets.pdf
  * on 5-February-2012.
  * XEX mode combines the random-access ability of CTR mode with the
  * bit-flipping attack resistance of ECB mode.
  *
  * This uses AES (see aes.c) as the underlying block cipher. Using AES in XEX
  * mode, with ciphertext stealing and with independent keys is sometimes
  * called "XTS-AES". But as long as the length of a
  * wallet record (#WALLET_RECORD_LENGTH) is a multiple of 16 bytes,
  * ciphertext stealing is not necessary. Thus the use
  * of AES in XEX mode here is identical in operation to XTS-AES.
  * As in XTS-AES, independent "tweak" and "encryption" keys are used. This
  * means that the combined key is 256 bits in length. But since this 256 bit
  * key is composed of two 128 bit keys, the final cipher still only
  * has 128 bits of security.
  *
  * This file is licensed as described by the file LICENCE.
  */

// Defining this will facilitate testing
//#define TEST

#ifdef TEST
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <string.h>
#endif // #ifdef TEST

#include "common.h"
#include "aes.h"
#include "prandom.h"
#include "hwinterface.h"
#include "endian.h"

/** Primary encryption key. */
static uint8_t nv_storage_encrypt_key[16];
/** The tweak key can be considered as a secondary, independent encryption
  * key. */
static uint8_t nv_storage_tweak_key[16];

/** Double a 128 bit integer under GF(2 ^ 128) with
  * reducing polynomial x ^ 128 + x ^ 7 + x ^ 2 + x + 1.
  * \param op1 The 128 bit integer to double. This should be an array of
  *            16 bytes representing the 128 bit integer in unsigned,
  *            little-endian multi-precision format.
  */
static void doubleInGF(uint8_t *op1)
{
	uint8_t i;
	uint8_t last_bit;
	uint8_t temp;

	last_bit = 0;
	for (i = 0; i < 16; i++)
	{
		temp = (uint8_t)(op1[i] & 0x80);
		op1[i] = (uint8_t)(op1[i] << 1);
		op1[i] |= last_bit;
		last_bit = (uint8_t)(temp >> 7);
	}
	last_bit = (uint8_t)(-(int)last_bit);
	// last_bit is now 0 if most-significant bit is 0, 0xff if most-significant
	// bit is 1.
	op1[0] = (uint8_t)(op1[0] ^ (0x87 & last_bit));
}

/** Combined XEX mode encrypt/decrypt, since they're almost the same.
  * See xexEncrypt() and xexDecrypt() for a description of what this does
  * and what each parameter is.
  * \param out For encryption, this will be the resulting ciphertext. For
  *            decryption, this will be the resulting plaintext.
  * \param in For encryption, this will be the source plaintext. For
  *           decryption, this will be the source ciphertext.
  * \param n See xexEncrypt().
  * \param seq See xexEncrypt().
  * \param tweak_key See xexEncrypt().
  * \param encrypt_key See xexEncrypt().
  * \param is_decrypt To decrypt, use a non-zero value for this parameter. To
  *                   encrypt, use zero for this parameter.
  */
static void xexEnDecrypt(uint8_t *out, uint8_t *in, uint8_t *n, uint8_t seq, uint8_t *tweak_key, uint8_t *encrypt_key, uint8_t is_decrypt)
{
	uint8_t expanded_key[EXPANDED_KEY_SIZE];
	uint8_t delta[16];
	uint8_t buffer[16];
	uint8_t i;

	aesExpandKey(expanded_key, tweak_key);
	aesEncrypt(delta, n, expanded_key);
	for (i = 0; i < seq; i++)
	{
		doubleInGF(delta);
	}
	for (i = 0; i < 16; i++)
	{
		buffer[i] = in[i];
	}
	xor16Bytes(buffer, delta);
	aesExpandKey(expanded_key, encrypt_key);
	if (is_decrypt)
	{
		aesDecrypt(out, buffer, expanded_key);
	}
	else
	{
		aesEncrypt(out, buffer, expanded_key);
	}
	xor16Bytes(out, delta);
}

/** Encrypt one 16 byte block using AES in XEX mode.
  * \param out The resulting ciphertext will be written to here. This must be
  *            a byte array with space for 16 bytes.
  * \param in The source plaintext. This must be a byte array containing the
  *           16 byte plaintext.
  * \param n A 128 bit number which specifies the number of the data
  *          unit (whatever a data unit is defined to be). This should be a
  *          byte array of 16 bytes, with the 128 bit number in unsigned,
  *          little-endian multi-precision format.
  * \param seq Specifies the block within the data unit.
  * \param tweak_key A 128 bit AES key.
  * \param encrypt_key Another 128 bit AES key. This must be independent of
  *                    tweak_key.
  * \warning Don't use seq = 0, as this presents a security
  *          vulnerability (albeit a convoluted one). For more details about
  *          the seq = 0 issue, see section 6 ("Security of XEX") of
  *          Rogaway's paper (reference at the top of this file).
  */
static void xexEncrypt(uint8_t *out, uint8_t *in, uint8_t *n, uint8_t seq, uint8_t *tweak_key, uint8_t *encrypt_key)
{
	xexEnDecrypt(out, in, n, seq, tweak_key, encrypt_key, 0);
}

/** Decrypt the 16 byte block using AES in XEX mode.
  * \param out The resulting plaintext will be written to here. This must be
  *            a byte array with space for 16 bytes.
  * \param in The source ciphertext. This must be a byte array containing the
  *           16 byte ciphertext.
  * \param n See xexEncrypt().
  * \param seq See xexEncrypt().
  * \param tweak_key See xexEncrypt().
  * \param encrypt_key See xexEncrypt().
  */
static void xexDecrypt(uint8_t *out, uint8_t *in, uint8_t *n, uint8_t seq, uint8_t *tweak_key, uint8_t *encrypt_key)
{
	xexEnDecrypt(out, in, n, seq, tweak_key, encrypt_key, 1);
}

/** Set the combined 256 bit encryption key.
  * This is compatible with getEncryptionKey().
  * \param in A 32 byte array specifying the 256 bit combined encryption key
  *           to use in XEX encryption/decryption operations.
  */
void setEncryptionKey(uint8_t *in)
{
	uint8_t i;
	for (i = 0; i < 16; i++)
	{
		nv_storage_encrypt_key[i] = in[i];
	}
	for (i = 0; i < 16; i++)
	{
		nv_storage_tweak_key[i] = in[i + 16];
	}
}

/** Get the combined 256 bit encryption key.
  * This is compatible with setEncryptionKey().
  * \param out A 32 byte array specifying where the current 256 bit combined
  *            encryption key will be written to.
  */
void getEncryptionKey(uint8_t *out)
{
	uint8_t i;
	for (i = 0; i < 16; i++)
	{
		out[i] = nv_storage_encrypt_key[i];
	}
	for (i = 0; i < 16; i++)
	{
		out[i + 16] = nv_storage_tweak_key[i];
	}
}

/** Check if the current combined encryption key is all zeroes. This has
  * implications for whether a wallet is considered encrypted or
  * not (see wallet.c).
  * \return Non-zero if the encryption key is not made up of all zeroes,
  *         zero if the encryption key is made up of all zeroes.
  */
uint8_t isEncryptionKeyNonZero(void)
{
	uint8_t r;
	uint8_t i;

	r = 0;
	for (i = 0; i < 16; i++)
	{
		r |= nv_storage_encrypt_key[i];
		r |= nv_storage_tweak_key[i];
	}
	return r;
}

/** Clear out memory which stores encryption keys.
  * In order to be sure that keys don't remain in RAM anywhere, you may also
  * need to clear out the space between the heap and the stack.
  */
void clearEncryptionKey(void)
{
	volatile uint8_t i;
	for (i = 0; i < 16; i++)
	{
		// Just to be sure
		nv_storage_tweak_key[i] = 0xff;
		nv_storage_encrypt_key[i] = 0xff;
	}
	for (i = 0; i < 16; i++)
	{
		nv_storage_tweak_key[i] = 0;
		nv_storage_encrypt_key[i] = 0;
	}
}

/** Wrapper around nonVolatileWrite() which also encrypts data
  * using xexEncrypt(). Because this uses encryption, it is much slower
  * than nonVolatileWrite(). The parameters and return values are identical
  * to that of nonVolatileWrite().
  * \param data A pointer to the data to be written.
  * \param address Byte offset specifying where in non-volatile storage to
  *                start writing to.
  * \param length The number of bytes to write.
  * \return See #NonVolatileReturnEnum for return values.
  * \warning Writes may be buffered; use nonVolatileFlush() to be sure that
  *          data is actually written to non-volatile storage.
  */
NonVolatileReturn encryptedNonVolatileWrite(uint8_t *data, uint32_t address, uint8_t length)
{
	uint32_t block_start;
	uint32_t block_end;
	uint8_t block_offset;
	uint8_t ciphertext[16];
	uint8_t plaintext[16];
	uint8_t n[16];
	uint8_t i;
	NonVolatileReturn r;

	block_start = address & 0xfffffff0;
	block_offset = (uint8_t)(address & 0x0000000f);
	block_end = (address + length - 1) & 0xfffffff0;

	for (i = 0; i < 16; i++)
	{
		n[i] = 0;
	}
	for (; block_start <= block_end; block_start += 16)
	{
		r = nonVolatileRead(ciphertext, block_start, 16);
		if (r != NV_NO_ERROR)
		{
			return r;
		}
		writeU32LittleEndian(n, block_start);
		xexDecrypt(plaintext, ciphertext, n, 1, nv_storage_tweak_key, nv_storage_encrypt_key);
		while (length && block_offset < 16)
		{
			plaintext[block_offset++] = *data++;
			length--;
		}
		block_offset = 0;
		xexEncrypt(ciphertext, plaintext, n, 1, nv_storage_tweak_key, nv_storage_encrypt_key);
		r = nonVolatileWrite(ciphertext, block_start, 16);
		if (r != NV_NO_ERROR)
		{
			return r;
		}
	}

	return NV_NO_ERROR;
}

/** Wrapper around nonVolatileRead() which also decrypts data
  * using xexDecrypt(). Because this uses encryption, it is much slower
  * than nonVolatileRead(). The parameters and return values are identical
  * to that of nonVolatileRead().
  * \param data A pointer to the buffer which will receive the data.
  * \param address Byte offset specifying where in non-volatile storage to
  *                start reading from.
  * \param length The number of bytes to read.
  * \return See #NonVolatileReturnEnum for return values.
  */
NonVolatileReturn encryptedNonVolatileRead(uint8_t *data, uint32_t address, uint8_t length)
{
	uint32_t block_start;
	uint32_t block_end;
	uint8_t block_offset;
	uint8_t ciphertext[16];
	uint8_t plaintext[16];
	uint8_t n[16];
	uint8_t i;
	NonVolatileReturn r;

	block_start = address & 0xfffffff0;
	block_offset = (uint8_t)(address & 0x0000000f);
	block_end = (address + length - 1) & 0xfffffff0;

	for (i = 0; i < 16; i++)
	{
		n[i] = 0;
	}
	for (; block_start <= block_end; block_start += 16)
	{
		r = nonVolatileRead(ciphertext, block_start, 16);
		if (r != NV_NO_ERROR)
		{
			return r;
		}
		writeU32LittleEndian(n, block_start);
		xexDecrypt(plaintext, ciphertext, n, 1, nv_storage_tweak_key, nv_storage_encrypt_key);
		while (length && block_offset < 16)
		{
			*data++ = plaintext[block_offset++];
			length--;
		}
		block_offset = 0;
	}

	return NV_NO_ERROR;
}

#ifdef TEST

static int succeeded;
static int failed;

static void skipWhiteSpace(FILE *f)
{
	int onechar;
	do
	{
		onechar = fgetc(f);
	} while ((onechar == ' ') || (onechar == '\t') || (onechar == '\n') || (onechar == '\r'));
	ungetc(onechar, f);
}

static void skipLine(FILE *f)
{
	int one_char;
	do
	{
		one_char = fgetc(f);
	} while (one_char != '\n');
}

static void print16(uint8_t *buffer)
{
	int i;
	for (i = 0; i < 16; i++)
	{
		printf("%02x", (int)buffer[i]);
	}
}

// If is_data_unit_seq_number is non-zero, this expects data unit sequence
// numbers (look for "DataUnitSeqNumber =" in the file) as the tweak
// value. Otherwise, this expects "i =" to specify the tweak value.
static void scanTestVectors(char *filename, int is_data_unit_seq_number)
{
	FILE *f;
	int test_number;
	int data_unit_length;
	int is_encrypt;
	int i;
	int j;
	int value;
	int seen_count;
	int test_failed;
	char buffer[100];
	uint8_t tweak_key[16];
	uint8_t encrypt_key[16];
	uint8_t tweak_value[16];
	uint8_t *plaintext;
	uint8_t *ciphertext;
	uint8_t *compare;

	f = fopen(filename, "r");
	if (f == NULL)
	{
		printf("Could not open %s, please get it \
(\"AES Known Answer Test (KAT) Vectors\") \
from http://csrc.nist.gov/groups/STM/cavp/#08\n", filename);
		printf("There should be two versions: one with 128-bit hex strings as the tweak\n");
		printf("value, and one with a \"data unit sequence number\" as the tweak value.\n");
		printf("Rename the one with 128-bit hex string tweak values \"XTSGenAES128i.rsp\"\n");
		printf("and rename the one with data unit sequence numbers \"XTSGenAES128d.rsp\".\n");
		exit(1);
	}

	test_number = 1;
	for (i = 0; i < 11; i++)
	{
		skipLine(f);
	}
	is_encrypt = 1;
	while (!feof(f))
	{
		// Check for [DECRYPT].
		skipWhiteSpace(f);
		seen_count = 0;
		while (!seen_count)
		{
			fgets(buffer, 6, f);
			skipLine(f);
			skipWhiteSpace(f);
			if (!strcmp(buffer, "[DECR"))
			{
				is_encrypt = 0;
			}
			else if (!strcmp(buffer, "COUNT"))
			{
				seen_count = 1;
			}
			else
			{
				printf("Expected \"COUNT\" or \"[DECR\"\n");
				exit(1);
			}
		}

		// Get data length.
		fgets(buffer, 15, f);
		if (strcmp(buffer, "DataUnitLen = "))
		{
			printf("Parse error; expected \"DataUnitLen = \"\n");
			exit(1);
		}
		fscanf(f, "%d", &data_unit_length);
		if ((data_unit_length <= 0) || (data_unit_length > 10000000))
		{
			printf("Error: got absurd data unit length %d\n", data_unit_length);
			exit(1);
		}
		skipWhiteSpace(f);

		if (data_unit_length & 0x7f)
		{
			// Skip tests which require ciphertext stealing, since ciphertext
			// stealing isn't implemented here (because it's not necessary).
			for (i = 0; i < 6; i++)
			{
				skipLine(f);
			}
		}
		else
		{
			data_unit_length >>= 3; // number of bits to number of bytes

			// Get key.
			fgets(buffer, 7, f);
			if (strcmp(buffer, "Key = "))
			{
				printf("Parse error; expected \"Key = \"\n");
				exit(1);
			}
			for (i = 0; i < 16; i++)
			{
				fscanf(f, "%02x", &value);
				encrypt_key[i] = (uint8_t)value;
			}
			for (i = 0; i < 16; i++)
			{
				fscanf(f, "%02x", &value);
				tweak_key[i] = (uint8_t)value;
			}
			skipWhiteSpace(f);

			// Get tweak value.
			if (is_data_unit_seq_number)
			{
				int n;

				fgets(buffer, 21, f);
				if (strcmp(buffer, "DataUnitSeqNumber = "))
				{
					printf("Parse error; expected \"DataUnitSeqNumber = \"\n");
					exit(1);
				}
				fscanf(f, "%d", &n);
				memset(tweak_value, 0, 16);
				tweak_value[0] = (uint8_t)n;
				tweak_value[1] = (uint8_t)(n >> 8);
				tweak_value[2] = (uint8_t)(n >> 16);
				tweak_value[3] = (uint8_t)(n >> 24);
			}
			else
			{
				fgets(buffer, 5, f);
				if (strcmp(buffer, "i = "))
				{
					printf("Parse error; expected \"i = \"\n");
					exit(1);
				}
				for (i = 0; i < 16; i++)
				{
					fscanf(f, "%02x", &value);
					tweak_value[i] = (uint8_t)value;
				}
			}
			skipWhiteSpace(f);

			plaintext = malloc(data_unit_length);
			ciphertext = malloc(data_unit_length);
			compare = malloc(data_unit_length);

			// Get plaintext/ciphertext.
			// The order is: plaintext, then ciphertext for encrypt.
			// The order is: ciphertext, then plaintext for decrypt.
			for (j = 0; j < 2; j++)
			{
				if (((is_encrypt) && (j == 0))
					|| ((!is_encrypt) && (j != 0)))
				{
					fgets(buffer, 6, f);
					if (strcmp(buffer, "PT = "))
					{
						printf("Parse error; expected \"PT = \"\n");
						exit(1);
					}
					for (i = 0; i < data_unit_length; i++)
					{
						fscanf(f, "%02x", &value);
						plaintext[i] = (uint8_t)value;
					}
				}
				else
				{
					fgets(buffer, 6, f);
					if (strcmp(buffer, "CT = "))
					{
						printf("Parse error; expected \"CT = \"\n");
						exit(1);
					}
					for (i = 0; i < data_unit_length; i++)
					{
						fscanf(f, "%02x", &value);
						ciphertext[i] = (uint8_t)value;
					}
				}
				skipWhiteSpace(f);
			} // end for (j = 0; j < 2; j++)

			// Do encryption/decryption and compare
			test_failed = 0;
			if (is_encrypt)
			{
				for (i = 0; i < data_unit_length; i += 16)
				{
					xexEncrypt(&(compare[i]), &(plaintext[i]), tweak_value, (uint8_t)(i >> 4), tweak_key, encrypt_key);
					if (memcmp(&(compare[i]), &(ciphertext[i]), 16))
					{
						test_failed = 1;
						break;
					}
				}
			}
			else
			{
				for (i = 0; i < data_unit_length; i += 16)
				{
					xexDecrypt(&(compare[i]), &(ciphertext[i]), tweak_value, (uint8_t)(i >> 4), tweak_key, encrypt_key);
					if (memcmp(&(compare[i]), &(plaintext[i]), 16))
					{
						test_failed = 1;
						break;
					}
				}
			}
			if (!test_failed)
			{
				succeeded++;
			}
			else
			{
				printf("Test %d failed\n", test_number);
				printf("Key: ");
				print16(encrypt_key);
				print16(tweak_key);
				printf("\nFirst 16 bytes of plaintext: ");
				print16(plaintext);
				printf("\nFirst 16 bytes of ciphertext: ");
				print16(ciphertext);
				printf("\n");
				failed++;
			}
			test_number++;
			free(plaintext);
			free(ciphertext);
			free(compare);
		}
	}
	fclose(f);
}

// Maximum address that a write to non-volatile storage will be
// Must be multiple of 128
#define MAX_ADDRESS 1024
// Number of read/write tests to do
#define NUM_RW_TESTS 100000

extern void initWalletTest(void);

int main(void)
{
	uint8_t what_storage_should_be[MAX_ADDRESS];
	uint8_t buffer[256];
	uint8_t one_key[32];
	int i;
	int j;

	initWalletTest();
	clearEncryptionKey();
	srand(42);
	succeeded = 0;
	failed = 0;
	scanTestVectors("XTSGenAES128i.rsp", 0);
	scanTestVectors("XTSGenAES128d.rsp", 1);

	for (i = 0; i < MAX_ADDRESS; i++)
	{
		what_storage_should_be[i] = (uint8_t)rand();
	}
	for (i = 0; i < MAX_ADDRESS; i += 128)
	{
		encryptedNonVolatileWrite(&(what_storage_should_be[i]), i, 128);
	}
	for (i = 0; i < MAX_ADDRESS; i += 128)
	{
		encryptedNonVolatileRead(buffer, i, 128);
		if (memcmp(&(what_storage_should_be[i]), buffer, 128))
		{
			printf("Storage mismatch in encryptedNonVolatileRead()\n");
			printf("Initial fill, address = 0x%08x, length = 128\n", i);
			failed++;
		}
		else
		{
			succeeded++;
		}
	}

	// Now read and write randomly, mirroring the reads and writes to the
	// what_storage_should_be array.
	for (i = 0; i < NUM_RW_TESTS; i++)
	{
		uint32_t address;
		uint8_t length;

		do
		{
			address = (uint32_t)(rand() & (MAX_ADDRESS - 1));
			length = (uint8_t)rand();
		} while ((address + length) > MAX_ADDRESS);
		if (rand() & 1)
		{
			// Write 50% of the time
			for (j = 0; j < length; j++)
			{
				buffer[j] = (uint8_t)rand();
			}
			memcpy(&(what_storage_should_be[address]), buffer, length);
			if (encryptedNonVolatileWrite(buffer, address, length) != NV_NO_ERROR)
			{
				printf("encryptedNonVolatileWrite() failed\n");
				printf("test number = %d, address = 0x%08x, length = %d\n", i, (int)address, (int)length);
				failed++;
			}
			else
			{
				succeeded++;
			}
		}
		else
		{
			// Read 50% of the time
			if (encryptedNonVolatileRead(buffer, address, length) != NV_NO_ERROR)
			{
				printf("encryptedNonVolatileRead() failed\n");
				printf("test number = %d, address = 0x%08x, length = %d\n", i, (int)address, (int)length);
				failed++;
			}
			else
			{
				if (memcmp(&(what_storage_should_be[address]), buffer, length))
				{
					printf("Storage mismatch in encryptedNonVolatileRead()\n");
					printf("test number = %d, address = 0x%08x, length = %d\n", i, (int)address, (int)length);
					failed++;
				}
				else
				{
					succeeded++;
				}
			}
		}
	}

	// Now change the encryption keys and try to obtain the contents of the
	// nonvolatile storage. The result should be mismatches everywhere.

	// Change only tweak key.
	memset(one_key, 0, 32);
	one_key[16] = 1;
	setEncryptionKey(one_key);
	for (i = 0; i < MAX_ADDRESS; i += 128)
	{
		encryptedNonVolatileRead(buffer, i, 128);
		if (memcmp(&(what_storage_should_be[i]), buffer, 128))
		{
			succeeded++;
		}
		else
		{
			printf("Storage match in encryptedNonVolatileRead() when using different tweak key\n");
			printf("Final run, address = 0x%08x, length = 128\n", i);
			failed++;
		}
	}

	// Change only (primary) encryption key.
	memset(one_key, 0, 32);
	one_key[0] = 1;
	setEncryptionKey(one_key);
	for (i = 0; i < MAX_ADDRESS; i += 128)
	{
		encryptedNonVolatileRead(buffer, i, 128);
		if (memcmp(&(what_storage_should_be[i]), buffer, 128))
		{
			succeeded++;
		}
		else
		{
			printf("Storage match in encryptedNonVolatileRead() when using different primary encryption key\n");
			printf("Final run, address = 0x%08x, length = 128\n", i);
			failed++;
		}
	}

	// Switch back to original, correct keys. All should be fine now.
	clearEncryptionKey();
	for (i = 0; i < MAX_ADDRESS; i += 128)
	{
		encryptedNonVolatileRead(buffer, i, 128);
		if (memcmp(&(what_storage_should_be[i]), buffer, 128))
		{
			printf("Storage mismatch in encryptedNonVolatileRead() when keys are okay\n");
			printf("Final run, address = 0x%08x, length = 128\n", i);
			failed++;
		}
		else
		{
			succeeded++;
		}
	}

	printf("Tests which succeeded: %d\n", succeeded);
	printf("Tests which failed: %d\n", failed);
	exit(0);
}

#endif // #ifdef TEST