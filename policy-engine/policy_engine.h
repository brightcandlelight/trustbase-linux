#ifndef POLICY_ENGINE_H_
#define POLICY_ENGINE_H_

#include "addons.h"
#include "plugins.h"
#include "query_queue.h"
#include "linked_list.h"

typedef struct policy_context_t {
	plugin_t* plugins;
	int plugin_count;
	addon_t* addons;
	int addon_count;
	double congress_threshold;
	queue_t* decider_queue;
	list_t* timeout_list;
} policy_context_t;

typedef struct thread_param_t {
	int plugin_id;
} thread_param_t;

int poll_schemes(uint32_t spid, uint64_t stptr, char* hostname, uint16_t port, unsigned char* cert_data, size_t len, char* client_hello, size_t client_hello_len, char* server_hello, size_t server_hello_len);
#endif
