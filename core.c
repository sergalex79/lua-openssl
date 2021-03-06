/*
// https://en.wikibooks.org/wiki/OpenSSL/Error_handling
// https://riptutorial.com/Download/openssl.pdf
// https://zakird.com/2013/10/13/certificate-parsing-with-openssl
// http://fm4dd.com/openssl/certcreate.htm
// http://fm4dd.com/openssl/certrenewal.htm
// https://security.stackexchange.com/questions/184845/how-to-generate-csrcertificate-signing-request-using-c-and-openssl
// https://curl.haxx.se/libcurl/c/usercertinmem.html
// https://doginthehat.com.au/2014/04/basic-openssl-rsa-encryptdecrypt-example-in-cocoa/
// www.opensource.apple.com/source/OpenSSL/OpenSSL-22/openssl/demos/x509/mkcert.c
// http://fm4dd.com/openssl/manual-ssl/
// http://fm4dd.com/openssl/pkcs12test.htm
// https://github.com/openssl/openssl/blob/master/test/v3nametest.c
// https://www.sslshopper.com/article-most-common-openssl-commands.html
// https://wiki.openssl.org/index.php/Main_Page
// https://ecn.io/pragmatically-generating-a-self-signed-certificate-and-private-key-using-openssl-d1753528e3d2
*/

#define LUA_LIB
#define _GNU_SOURCE

#include <errno.h>
#include <lauxlib.h>
#include <lua.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <openssl/buffer.h>

#if LUA_VERSION_NUM < 502
#define luaL_newlib(L,l) (lua_newtable(L), luaL_register(L,NULL,l))
#define luaL_setfuncs(L,l,n) (assert(n==0), luaL_register(L,NULL,l))
#define luaL_checkunsigned(L,n) luaL_checknumber(L,n)
#endif

#if LUA_VERSION_NUM >= 503
#ifndef luaL_checkunsigned
#define luaL_checkunsigned(L,n) ((lua_Unsigned)luaL_checkinteger(L,n))
#endif
#endif

#ifdef NO_CHECK_UDATA
#define checkudata(L,i,tname) lua_touserdata(L, i)
#else
#define checkudata(L,i,tname) luaL_checkudata(L, i, tname)
#endif

#define lua_boxpointer(L,u) \
		(*(void **) (lua_newuserdata(L, sizeof(void *))) = (u))

#define lua_unboxpointer(L,i,tname) \
		(*(void **) (checkudata(L, i, tname)))

/* Max Lua arguments for function */
#define MAXVARS 200

void err_descr_to_stderr(const char *err_patern) {
	char buffer[120];
	ERR_error_string(ERR_get_error(), buffer);
	fprintf(stderr, "%s due to: %s\n", err_patern, buffer);
}

int init_crypto(lua_State *L) {
	unsigned int argc = lua_gettop(L);
	if (argc > 0) {
		fprintf(stderr, "you must not pass arguments\n");
		return 0;
	}

	OpenSSL_add_all_algorithms();
	ERR_load_BIO_strings();
	ERR_load_crypto_strings();

	return 0;
}

int csr_crt(lua_State *L) {
	uint rc = 0;

	unsigned int argc = lua_gettop(L);
	if (argc < 3) {
		fprintf(stderr, "you must pass one argument: (priv_key, csr)!\n");
		goto __error;
	}

	if (lua_isstring(L, 1) != 1) {
		fprintf(stderr, "first argument must be string: private key!\n");
		goto __error;
	}
	size_t pkey_len = 0;
	const char *pkey = luaL_checklstring(L, 1, &pkey_len);
	if (pkey_len == 0) {
		fprintf(stderr, "pkey length should greater then zero!\n");
		goto __error;
	}

	if (lua_isstring(L, 2) != 1) {
		fprintf(stderr, "second argument must be string: crt!\n");
		goto __error;
	}
	size_t crt_len = 0;
	const char *crt = luaL_checklstring(L, 2, &crt_len);
	if (crt_len == 0) {
		fprintf(stderr, "crt length should greater then zero!\n");
		goto __error;
	}

	if (lua_isstring(L, 3) != 1) {
		fprintf(stderr, "third argument must be string: csr!\n");
		goto __error;
	}
	size_t csr_len = 0;
	const char *csr = luaL_checklstring(L, 3, &csr_len);
	if (csr_len == 0) {
		fprintf(stderr, "csr length should greater then zero!\n");
		goto __error;
	}


	// load pkey
	char *password = "replace_me";
	BIO *pkeybio = BIO_new_mem_buf(pkey, pkey_len);
	RSA *rsa = PEM_read_bio_RSAPrivateKey(pkeybio, NULL, NULL, password);
	if (rsa == NULL) {
		err_descr_to_stderr("Failed to create key bio");
		goto __error;
	}

	// 4. set public key of x509 req
	EVP_PKEY *pKey = EVP_PKEY_new();
	EVP_PKEY_assign_RSA(pKey, rsa);


	// load pca
	BIO *cacertbio = BIO_new_mem_buf(crt, crt_len);
	X509 *cacert = PEM_read_bio_X509(cacertbio, NULL, NULL, NULL);

	// load csr
	/* ---------------------------------------------------------- *
	 * Load the request data in a BIO, then in a x509_REQ struct. *
	 * ---------------------------------------------------------- */
	BIO *reqbio = BIO_new_mem_buf(csr, csr_len);
	X509_REQ *certreq = NULL;
	if (! (certreq = PEM_read_bio_X509_REQ(reqbio, NULL, NULL, NULL))) {
		err_descr_to_stderr("Error can't read X509 request data into memory");
		goto __error;
	}

	// create certificate
	/* --------------------------------------------------------- *
	 * Build Certificate with data from request                  *
	 * ----------------------------------------------------------*/
	X509 *newcert = NULL;
	if (! (newcert=X509_new())) {
		err_descr_to_stderr("Error creating new X509 object");
		goto __error;
	}

	if (X509_set_version(newcert, 2) != 1) {
		err_descr_to_stderr("Error setting certificate version");
		goto __error;
	}

	if (X509_set_pubkey(newcert, pKey) != 1) {
		err_descr_to_stderr("X509_set_pubkey");
		goto __error;
	}

	ASN1_INTEGER  *aserial = NULL;
	aserial=ASN1_INTEGER_new();
	ASN1_INTEGER_set(aserial, 0);
	if (! X509_set_serialNumber(newcert, aserial)) {
		err_descr_to_stderr("Error setting serial number of the certificate");
		goto __error;
	}

	/* --------------------------------------------------------- *
	 * Extract the subject name from the request                 *
	 * ----------------------------------------------------------*/
	X509_NAME *name;
	if (! (name = X509_REQ_get_subject_name(certreq))) {
		err_descr_to_stderr("Error getting subject from cert request");
		goto __error;
	}

	/* --------------------------------------------------------- *
	 * Set the new certificate subject name                      *
	 * ----------------------------------------------------------*/
	if (X509_set_subject_name(newcert, name) != 1) {
		err_descr_to_stderr("Error setting subject name of certificate");
		goto __error;
	}

	/* --------------------------------------------------------- *
	 * Extract the subject name from the signing CA cert         *
	 * ----------------------------------------------------------*/
	if (! (name = X509_get_subject_name(cacert))) {
		err_descr_to_stderr("Error getting subject from CA certificate");
		goto __error;
	}

	/* --------------------------------------------------------- *
	 * Set the new certificate issuer name                       *
	 * ----------------------------------------------------------*/
	if (X509_set_issuer_name(newcert, name) != 1) {
		err_descr_to_stderr("Error setting issuer name of certificate");
		goto __error;
	}

	/* --------------------------------------------------------- *
	 * Extract the public key data from the request              *
	 * ----------------------------------------------------------*/
	EVP_PKEY *req_pubkey;
	if (! (req_pubkey=X509_REQ_get_pubkey(certreq))) {
		err_descr_to_stderr("Error unpacking public key from request");
		goto __error;
	}

	/* --------------------------------------------------------- *
	 * Optionally: Use the public key to verify the signature    *
	 * ----------------------------------------------------------*/
	if (X509_REQ_verify(certreq, req_pubkey) != 1) {
		err_descr_to_stderr("Error verifying signature on request");
		goto __error;
	}

	/* --------------------------------------------------------- *
	 * Set the new certificate public key                        *
	 * ----------------------------------------------------------*/
	if (X509_set_pubkey(newcert, req_pubkey) != 1) {
		err_descr_to_stderr("Error setting public key of certificate");
		goto __error;
	}

	/* ---------------------------------------------------------- *
	 * Set X509V3 start date (now) and expiration date (+365 days)*
	 * -----------------------------------------------------------*/
	if (! (X509_gmtime_adj(X509_get_notBefore(newcert),0))) {
		err_descr_to_stderr("Error setting start time");
		goto __error;
	}

	long valid_secs = 31536000;

	if(! (X509_gmtime_adj(X509_get_notAfter(newcert), valid_secs))) {
		err_descr_to_stderr("Error setting expiration time");
		goto __error;
	}

	/* ----------------------------------------------------------- *
	 * Add X509V3 extensions                                       *
	 * ------------------------------------------------------------*/
	X509V3_CTX                   ctx;
	X509V3_set_ctx(&ctx, cacert, newcert, NULL, NULL, 0);
	// X509_EXTENSION *ext;

	/* ----------------------------------------------------------- *
	 * Set digest type, sign new certificate with CA's private key *
	 * ------------------------------------------------------------*/
	EVP_MD                       const *digest = NULL;
	digest = EVP_sha256();

	if (! X509_sign(newcert, pKey, digest)) {
		err_descr_to_stderr("Error signing the new certificate");
		goto __error;
	}

	/* ------------------------------------------------------------ *
	 *  print the certificate                                       *
	 * -------------------------------------------------------------*/
	BIO  *outbio = BIO_new(BIO_s_mem());
	if (! PEM_write_bio_X509(outbio, newcert)) {
		err_descr_to_stderr("Error printing the signed certificate");
		goto __error;
	}

	BUF_MEM *bptr;
	BIO_get_mem_ptr(outbio, &bptr);
	lua_pushlstring(L, bptr->data, bptr->length + 1);
	BUF_MEM_free(bptr);

	// all ok set RC to 1
	rc = 1;

__error:
	// private key and buffer free
	if (pkeybio != NULL) {
		BIO_free_all(pkeybio);
	}
	if (pKey != NULL) {
		EVP_PKEY_free(pKey);
	}

	// CA certificate, CA pub key and buffer free
	if (cacertbio != NULL) {
		BIO_free_all(cacertbio);
	}
	if (cacert != NULL) {
		X509_free(cacert);
	}
	if (req_pubkey != NULL) {
		EVP_PKEY_free(req_pubkey);
	}

	// CSR and buffer free
	if (reqbio != NULL) {
		BIO_free_all(reqbio);
	}
	if (certreq != NULL) {
		X509_REQ_free(certreq);
	}

	if (newcert != NULL) {
		// signed CRT free
		X509_free(newcert);
	}

	if (outbio != NULL) {
		BIO_free_all(outbio);
	}

	return rc;
}

// Register library using this array
static const struct luaL_Reg OpenSSLLib[] = {
//    {"gen_rsa_key", gen_rsa_key},
//    {"gen_csr", gen_csr},
//    {"gen_crt", gen_crt},
	{"init_crypto", init_crypto},
    {"csr_crt", csr_crt},
    {NULL, NULL}
};

// LUALIB_API int luaopen_openssl_core(lua_State *L) {
LUALIB_API int luaopen_core(lua_State *L) {
  luaL_newlib(L, OpenSSLLib);
  return 1;
}
