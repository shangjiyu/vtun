#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "vtun.h"
#include "linkfd.h"

#ifdef HAVE_SODIUM
#include <sodium.h>

#define crypto_aead_NPUBBYTES crypto_aead_chacha20poly1305_IETF_NPUBBYTES
#define crypto_aead_ABYTES    crypto_aead_chacha20poly1305_ABYTES
#define crypto_aead_KEYBYTES  crypto_aead_chacha20poly1305_KEYBYTES

#define MESSAGE_MAX_SIZE          VTUN_FRAME_SIZE
#define CIPHERTEXT_ABYTES         (crypto_aead_ABYTES + crypto_aead_NPUBBYTES)
#define CIPHERTEXT_MAX_SIZE       MESSAGE_MAX_SIZE
#define CIPHERTEXT_MAX_TOTAL_SIZE (CIPHERTEXT_MAX_SIZE + CIPHERTEXT_ABYTES)

#define MINIMUM_DATE 1444341043UL
#define SLEEP_WHEN_CLOCK_IS_OFF 10

typedef struct CryptoCtx {
    unsigned char *key;
    unsigned char *ciphertext;
    unsigned char *message;
    unsigned char *nonce;
    unsigned char *previous_decrypted_nonce;
} CryptoCtx;

static CryptoCtx ctx;

static int
init_nonce(unsigned char *nonce, size_t nonce_size)
{
    time_t now;

    if (nonce_size < 5) {
        return -1;
    }
    time(&now);
    if (now < MINIMUM_DATE) {
        sleep(SLEEP_WHEN_CLOCK_IS_OFF);
        randombytes_buf(nonce, nonce_size);
    } else {
        randombytes_buf(nonce, nonce_size - 3);
        nonce[nonce_size - 1] = (unsigned char) (now >> 22);
        nonce[nonce_size - 2] = (unsigned char) (now >> 14);
        nonce[nonce_size - 3] = (unsigned char) (now >> 6);
        nonce[nonce_size - 4] =
            (unsigned char) (now << 2) ^ (nonce[nonce_size - 4] & 0x3);
    }
    if (vtun.svr != 0) {
        nonce[nonce_size - 1] |= 0x80;
    } else {
        nonce[nonce_size - 1] &= ~0x80;
    }
    return 0;
}

static int
alloc_encrypt(struct vtun_host *host)
{
    ctx.key = sodium_malloc(crypto_aead_KEYBYTES);
    ctx.message = sodium_malloc(MESSAGE_MAX_SIZE);
    ctx.ciphertext = sodium_malloc(CIPHERTEXT_MAX_TOTAL_SIZE);
    ctx.nonce = sodium_malloc(crypto_aead_NPUBBYTES);
    ctx.previous_decrypted_nonce = sodium_malloc(crypto_aead_NPUBBYTES);
    if (host->key == NULL || ctx.key == NULL || ctx.message == NULL ||
        ctx.ciphertext == NULL || ctx.ciphertext == NULL || ctx.nonce == NULL ||
        ctx.previous_decrypted_nonce == NULL) {
        abort();
    }
    if (init_nonce(ctx.nonce, crypto_aead_NPUBBYTES) != 0) {
        return -1;
    }
    memset(ctx.previous_decrypted_nonce, 0, crypto_aead_NPUBBYTES);
    memcpy(ctx.key, host->key, sizeof(host->key));
    sodium_free(host->key);
    host->key = NULL;

    return 0;
}

static int
free_encrypt(void)
{
    sodium_free(ctx.message);
    sodium_free(ctx.ciphertext);
    sodium_free(ctx.nonce);
    sodium_free(ctx.previous_decrypted_nonce);

    return 0;
}

static int
encrypt_buf(int message_len_, char *message_, char ** const ciphertext_p)
{
    const unsigned char *message = (const unsigned char *) message_;
    const size_t         message_len = (size_t) message_len_;
    unsigned long long   ciphertext_len;

    if (message_len_ < 0 || message_len > MESSAGE_MAX_SIZE) {
        return -1;
    }
    crypto_aead_chacha20poly1305_ietf_encrypt(ctx.ciphertext, &ciphertext_len,
                                          message, message_len,
                                          NULL, 0ULL,
                                          NULL, ctx.nonce,
                                          ctx.key);
    memcpy(ctx.ciphertext + message_len + crypto_aead_ABYTES,
           ctx.nonce, crypto_aead_NPUBBYTES);
    sodium_increment(ctx.nonce, crypto_aead_NPUBBYTES);
    *ciphertext_p = (char *) ctx.ciphertext;

    return (int) ciphertext_len + crypto_aead_NPUBBYTES;
}

static int
decrypt_buf(int ciphertext_len_, char *ciphertext_, char ** const message_p)
{
    const unsigned char *ciphertext = (const unsigned char *) ciphertext_;
    const unsigned char *nonce;
    size_t               ciphertext_len = (size_t) ciphertext_len_;
    unsigned long long   message_len;

    if (ciphertext_len_ < CIPHERTEXT_ABYTES ||
        ciphertext_len > CIPHERTEXT_MAX_SIZE) {
        return -1;
    }
    ciphertext_len -= crypto_aead_NPUBBYTES;
    nonce = ciphertext + ciphertext_len;
    if (sodium_compare(nonce, ctx.previous_decrypted_nonce, crypto_aead_NPUBBYTES) <= 0 ||
	crypto_aead_chacha20poly1305_ietf_decrypt(ctx.message, &message_len, NULL,
                                              ciphertext, ciphertext_len,
                                              NULL, 0ULL, nonce,
                                              ctx.key) != 0) {
        return -1;
    }
    memcpy(ctx.previous_decrypted_nonce, nonce, crypto_aead_NPUBBYTES);
    *message_p = (char *) ctx.message;

    return (int) message_len;
}

struct lfd_mod lfd_encrypt = {
     "Encryptor",
     alloc_encrypt,
     encrypt_buf,
     NULL,
     decrypt_buf,
     NULL,
     free_encrypt,
     NULL,
     NULL
};

#else  /* HAVE_SODIUM */

static int
no_encrypt(struct vtun_host *host)
{
     vtun_syslog(LOG_INFO, "Encryption is not supported");
     return -1;
}

struct lfd_mod lfd_encrypt = {
     "Encryptor",
     no_encrypt, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

#endif
