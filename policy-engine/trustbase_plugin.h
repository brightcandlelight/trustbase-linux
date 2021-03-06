
#ifndef TRUSTBASE_PLUGIN_H_
#define TRUSTBASE_PLUGIN_H_

#include <stdint.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include "tb_logging.h"

#define PLUGIN_RESPONSE_ERROR	-1
#define PLUGIN_RESPONSE_VALID	1
#define PLUGIN_RESPONSE_INVALID	0
#define PLUGIN_RESPONSE_ABSTAIN	2

typedef struct query_data_t {
	int id;
	char* hostname;
	uint16_t port;
	STACK_OF(X509)* chain;
	unsigned char* raw_chain;
	size_t raw_chain_len;
	char* client_hello;
	size_t client_hello_len;
	char* server_hello;
	size_t server_hello_len;
} query_data_t;

typedef struct init_data_t {
	int plugin_id;
	char* plugin_path;
	int(*callback)(int plugin_id, int query_id, int plugin_response);
	int (*tblog)(tblog_level_t level, const char* format, ...);
} init_data_t;

#endif
