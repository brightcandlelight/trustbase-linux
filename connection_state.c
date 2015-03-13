#include <linux/hashtable.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include "connection_state.h"
#include "secure_handshake_parser.h"
#include "utils.h"

extern unsigned int allocsminusfrees;
#define HASH_TABLE_BITSIZE	8
DEFINE_HASHTABLE(conn_table, HASH_TABLE_BITSIZE);

void th_conn_state_free(conn_state_t* conn_state) {
	kfree(conn_state->send_buf);
	kfree(conn_state->recv_buf);
	kfree(conn_state);
	allocsminusfrees--;
	return;
}

conn_state_t* th_conn_state_get(pid_t pid, int fd) {
	conn_state_t* conn_state = NULL;
	conn_state_t* conn_state_it;
        hash_for_each_possible(conn_table, conn_state_it, hash, pid ^ fd) {
                if (conn_state_it->pid == pid && conn_state_it->socketfd == fd) {
                        conn_state = conn_state_it;
                        break;
                }
        }
	return conn_state;
}


void th_conn_state_create(pid_t pid, unsigned int socketfd) {
	conn_state_t* new_conn_state = NULL;
	if ((new_conn_state = kmalloc(sizeof(conn_state_t), GFP_KERNEL)) == NULL) {
		printk(KERN_ALERT "kmalloc failed when creating connection state");
	}
	allocsminusfrees++;
	new_conn_state->pid = pid;
	new_conn_state->socketfd = socketfd;
	new_conn_state->key = pid ^ socketfd;
	//if ((new_conn_state->buf = kmalloc(TH_TLS_RECORD_HEADER_SIZE, GFP_KERNEL)) == NULL) {
	//	printk(KERN_ALERT "kmalloc failed when creating connection state buffer");
	//}
	new_conn_state->state = TLS_CLIENT_UNKNOWN;
	new_conn_state->send_buf = NULL;
	new_conn_state->recv_buf = NULL;
	new_conn_state->send_buf_length = 0;
	new_conn_state->recv_buf_length = 0;
	new_conn_state->send_bytes_to_read = TH_TLS_HANDSHAKE_IDENTIFIER_SIZE;
	new_conn_state->recv_bytes_to_read = TH_TLS_HANDSHAKE_IDENTIFIER_SIZE;

	// Add to hash table
	hash_add(conn_table, &new_conn_state->hash, new_conn_state->key);
	return;
}

void th_conn_state_print_all(void) {
	int bkt;
	conn_state_t* conn_state_it;
	hash_for_each(conn_table, bkt, conn_state_it, hash) {
		printk(KERN_INFO "bucket [%d] has pid value %d and socket value %d", bkt, conn_state_it->pid, conn_state_it->socketfd);
	}
	return;
}

void th_conn_state_init_all(void) {
	allocsminusfrees = 0;
	hash_init(conn_table);
	return;
}

void th_conn_state_free_all(void) {
	int bkt;
	conn_state_t* conn_state_it;
	struct hlist_node tmp;
	struct hlist_node* tmpptr = &tmp;
	hash_for_each_safe(conn_table, bkt, tmpptr, conn_state_it, hash) {
		printk(KERN_INFO "Deleting things in bucket [%d] with pid value %d and socket value %d", bkt, conn_state_it->pid, conn_state_it->socketfd);
		hash_del(&conn_state_it->hash);
		th_conn_state_free(conn_state_it);
	}
	printk(KERN_INFO "kallocs minus kfrees: %i", allocsminusfrees);
	return;
}

int th_conn_state_delete(pid_t pid, unsigned int fd) {
	int found = 0;
	conn_state_t* conn_state_it;
        hash_for_each_possible(conn_table, conn_state_it, hash, pid ^ fd) {
		if (conn_state_it->pid == pid && conn_state_it->socketfd == fd) {
			hash_del(&conn_state_it->hash);
			th_conn_state_free(conn_state_it);
			found = 1;
			break;
		}
	}
	return found;
}

