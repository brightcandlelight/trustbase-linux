#include <linux/kernel.h>
#include <linux/sched.h> // For current (pointer to task)
#include <linux/pid.h> // For pid_t
#include <linux/socket.h> // For socket structures
#include <linux/slab.h> // For memory allocation
#include <linux/tcp.h> // For TCP structures

#include "interceptor.h"
#include "connection_state.h" // For accessing handler functions
#include "secure_handshake_parser.h"
#include "utils.h" // For debug

// TCP IPv6-specific reference functions
extern int (*ref_tcp_v4_connect)(struct sock *sk, struct sockaddr *uaddr, int addr_len);
// TCP IPv4-specific wrapper functions
extern int (*ref_tcp_v6_connect)(struct sock *sk, struct sockaddr *uaddr, int addr_len);
// TCP General reference functions
extern int (*ref_tcp_disconnect)(struct sock *sk, int flags);
extern void (*ref_tcp_close)(struct sock *sk, long timeout);
extern int (*ref_tcp_sendmsg)(struct kiocb *iocb, struct sock *sk, struct msghdr *msg, size_t size);
extern int (*ref_tcp_recvmsg)(struct kiocb *iocb, struct sock *sk, struct msghdr *msg, size_t len, int nonblock, int flags, int *addr_len);

conn_state_ops_t* h_ops;

// Wrapper definitions
int new_tcp_v4_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len) {
	int ret;
	struct socket* sock;
	sock = sk->sk_socket;
	ret = ref_tcp_v4_connect(sk, uaddr, addr_len);
	//printk(KERN_INFO "TCP over IPv4 connection detected");
	//print_call_info(sock, "TCP IPv4 connect");
	conn_state_create(current->pid, sock, h_ops);
	return ret;
}

int new_tcp_v6_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len) {
	int ret;
	struct socket* sock;
	sock = sk->sk_socket;
	ret = ref_tcp_v6_connect(sk, uaddr, addr_len);
	//printk(KERN_INFO "TCP over IPv6 connection detected");
	//print_call_info(sock, "TCP IPv6 connect");
	conn_state_create(current->pid, sock, h_ops);
	return ret;
}

int new_tcp_disconnect(struct sock *sk, int flags) {
	int ret;
	ret = ref_tcp_disconnect(sk, flags);
	//printk(KERN_INFO "TCP disconnect detected");
	return ret;
}

void new_tcp_close(struct sock *sk, long timeout) {
	struct socket* sock;
	sock = sk->sk_socket;
	if (th_conn_state_delete(current->pid, sock)) {
		//print_call_info(sock, "TCP close");
	}
	ref_tcp_close(sk, timeout);
	return;
}

int new_tcp_sendmsg(struct kiocb *iocb, struct sock *sk, struct msghdr *msg, size_t size) {
	conn_state_t* conn_state;
	struct socket* sock;
	int real_ret;
	struct iovec iov;
	struct msghdr kmsg;
	void* new_data;
	mm_segment_t oldfs;
	sock = sk->sk_socket;

	// Adopt default kernel behavior if we're not monitoring this connection
	if ((conn_state = th_conn_state_get(current->pid, sock)) == NULL) {
		return ref_tcp_sendmsg(iocb, sk, msg, size);
	}

	// Copy attributes of existing message into our custom one
	kmsg = *msg;
	iov.iov_len = 0; // will be set later
	iov.iov_base = NULL; // will be set later
	kmsg.msg_iov = &iov;

	// Pointer to data being sent by user.
	new_data = msg->msg_iov->iov_base;

	// 0) If last send attempt was an error, don't copy or update state
	if (conn_state->queued_send_ret > 0) {
		// 1) Copy data from user to our connection state buffer
		if (conn_state->ops->send_to_proxy(conn_state->send_state, new_data, size) != 0) {
			printk(KERN_ALERT "failed to copy data to connstate buffer");
			// XXX delete this connection, we can't handle it
			// Do we try to send existing buffer data?
			// Abort by calling original functionality
			return ref_tcp_sendmsg(iocb, sk, msg, size);
		}
		// 2) Update handler's state now that it has new data
		if (conn_state->ops->update_send_state(conn_state->send_state) != 0) {
			printk(KERN_ALERT "failed to update state");
			// XXX delete this connection, we can't handle it
			// Do we try to send existing buffer data?
			// Abort by calling original functionality
			return ref_tcp_sendmsg(iocb, sk, msg, size);
		}
	}
	else {
		// This branch indicates that the last send operation resulted in an
		// error.
		//
		// By skipping the copy and update phases in this branch we
		// effectively assume that the data being sent after an error
		// is the same as the previous time.
		//
		// XXX We could set up something here to verify that the 
		// data sent by the client this time around is the same as last time
		// but I'm not sure we have to.  Only a dumb programmer would alter
		// the contents of his buffer in between send attempts.
	}

	// 3) Have handler tell us what we should forward
	//    This will be the same as last time if an error occurred
	conn_state->ops->fill_send_buffer(conn_state->send_state, &iov.iov_base, &iov.iov_len);

	// 4) Forward what handler told us to forward, if anything
	if (iov.iov_len <= 0) { //should never really be negative
		if (conn_state->ops->num_send_bytes_to_forward(conn_state->send_state) == 0 && conn_state->ops->get_send_state(conn_state->send_state) == IRRELEVANT) {
			//print_call_info(sock, "No longer interested in socket, ceasing monitoring");
			th_conn_state_delete(current->pid, sock); 
	        }
		// Tell the user we sent everything he wanted
		return size;
	}
	// Use real tcp_sendmsg call to transmit
	// but do it via the persona of the kernel
	oldfs = get_fs();
	set_fs(KERNEL_DS);
	real_ret = ref_tcp_sendmsg(iocb, sk, &kmsg, iov.iov_len);
	set_fs(oldfs);
	// Record result
	conn_state->queued_send_ret = real_ret;
	if (real_ret > 0) {
		conn_state->ops->inc_send_bytes_forwarded(conn_state->send_state, real_ret);
	}
	if (real_ret != iov.iov_len) {
		printk(KERN_ALERT "Kernel couldn't send everything we wanted to");
		if (msg->msg_flags & MSG_DONTWAIT) { // nonblocking IO
			// This forces a resend (dont need to delete here because we're
			// still interested in socket, clearly)
			conn_state->queued_send_ret = -EAGAIN;
			return -EAGAIN;
		}
		else { // blocking IO
			// loop here to retry because this might be the last time we're ever called
			while (conn_state->ops->num_send_bytes_to_forward(conn_state->send_state) > 0) {
				// Ask handler to update our pointer and length again
				conn_state->ops->fill_send_buffer(conn_state->send_state, &iov.iov_base, &iov.iov_len);
				// Attempt send again
				real_ret = ref_tcp_sendmsg(iocb, sk, &kmsg, iov.iov_len);
				// Record bytes sent
				conn_state->ops->inc_send_bytes_forwarded(conn_state->send_state, real_ret);
			}
		}
	}
	// If handler doesn't care about connection anymore then delete it
	if (conn_state->ops->num_send_bytes_to_forward(conn_state->send_state) == 0 && conn_state->ops->get_send_state(conn_state->send_state) == IRRELEVANT) {
		//print_call_info(sock, "No longer interested in socket, ceasing monitoring");
		th_conn_state_delete(current->pid, sock); 
        }
	// Just tell the user we sent everything he wanted
	// or an error code, if an error occurred
	return real_ret > 0 ? size : real_ret;

/*
	// Passthrough version	
	real_ret = ref_tcp_sendmsg(iocb, sk, msg, size);
	if (real_ret < 0) {
		return real_ret;
	}
	if ((conn_state = th_conn_state_get(current->pid, sock)) == NULL) {
		return real_ret;
	}
	th_parse_comm(current->pid, sock, (char*)msg->msg_iov->iov_base, real_ret, TH_SEND);
	if (conn_state->send_state.state == IRRELEVANT) {
		print_call_info(sock, "No longer interested in socket, ceasing monitoring");
		th_conn_state_delete(current->pid, sock); 
	}
	return real_ret;
*/
}

int new_tcp_recvmsg(struct kiocb *iocb, struct sock *sk, struct msghdr *msg, size_t len, int nonblock, int flags, int *addr_len) {
	int ret;
	mm_segment_t oldfs;
	struct iovec iov;
	struct msghdr kmsg;
	void* buffer;
	struct socket* sock;
	conn_state_t* conn_state;
	int bytes_to_copy;
	int bytes_sent;
	int b_to_forward;
	int b_to_read;
	sock = sk->sk_socket;

 	// New way
	// Early breakout if we aren't monitoring this connection
	if ((conn_state = th_conn_state_get(current->pid, sock)) == NULL) {
		ret = ref_tcp_recvmsg(iocb, sk, msg, len, nonblock, flags, addr_len);
		return ret;
	}

	bytes_sent = 0;
	// 1) Place into user's buffer any data already marked for fowarding
	//    up to maxiumum user is requesting (len)
	b_to_forward = conn_state->ops->num_recv_bytes_to_forward(conn_state->recv_state);
	if (b_to_forward > 0) {
		bytes_to_copy = b_to_forward > len ? len : b_to_forward;
		if (conn_state->ops->copy_to_user(conn_state->recv_state, (void __user *)msg->msg_iov->iov_base, bytes_to_copy) != 0) {
			printk(KERN_ALERT "failed to copy what we wanted to");
			// XXX how do we fail here?
		}
		bytes_sent += bytes_to_copy;
		conn_state->ops->inc_recv_bytes_forwarded(conn_state->recv_state, bytes_sent);
	}

	if (bytes_sent)
		printk(KERN_ALERT "I sent the user %d cached bytes", bytes_sent);
	// 2) If we've already given the user everything he wants, end
	if (bytes_sent == len) {
		return len;
	}

	// If we've not sent anything yet and the socket was closed last time
	// we actually read, then delete state and return
	if (bytes_sent == 0 && conn_state->queued_recv_ret == 0) {
		th_conn_state_delete(current->pid, sock);
		return 0;
	}
	if (bytes_sent == 0 && conn_state->queued_recv_ret < 0) {
		ret = conn_state->queued_recv_ret;
		conn_state->queued_recv_ret = 1; // pretend no error for next time
		return ret;
	}


	// If we don't care to read any more bytes for this socket, stop now
	if (conn_state->ops->bytes_to_read(conn_state->recv_state) == 0) {
		if (bytes_sent > 0) {
			return bytes_sent;
		}
		else {
			th_conn_state_delete(current->pid, sock);
			return ref_tcp_recvmsg(iocb, sk, msg, len, nonblock, flags, addr_len);
		}
	}

	// At this point bytes_to_forward should be zero,
	// queued_recv_ret should be positive, and bytes_to_read
	// should be positive
	
	
	// 3) Attempt to get more data from external sources
	while (conn_state->ops->num_recv_bytes_to_forward(conn_state->recv_state) == 0) {
		kmsg = *msg;
		kmsg.msg_iov = &iov;
		b_to_read = conn_state->ops->bytes_to_read(conn_state->recv_state);
	        buffer = kmalloc(b_to_read, GFP_KERNEL);
		iov.iov_len = b_to_read;
		iov.iov_base = buffer;

		oldfs = get_fs();
		set_fs(KERNEL_DS);
		ret = ref_tcp_recvmsg(iocb, sk, &kmsg, iov.iov_len, nonblock, flags, addr_len);
		//printk(KERN_ALERT "real ret is %d", ret);
		if (ret == -EIOCBQUEUED) {
			ret = wait_on_sync_kiocb(iocb);
		}
		set_fs(oldfs);
		
		// 4) if operation failed then just return what we've sent so far
		//    or the error code
		conn_state->queued_recv_ret = ret;
		if (ret <= 0) {
			if (bytes_sent > 0) {
				// error code is cached for next time
				return bytes_sent; 
			}
			else {
				// Pretend no error for next time since we're
				// sending it now
				conn_state->queued_recv_ret = 1;
				return ret;
			}
		}

		// 5) If operation succeeded then copy to state and update state
		if (conn_state->ops->send_to_proxy(conn_state->recv_state, buffer, ret) != 0) {
			printk(KERN_ALERT "failed to copy to recv state");
			// XXX how do we fail here?
		}
		kfree(buffer);
		if (conn_state->ops->update_recv_state(conn_state->recv_state) != 0) {
			printk(KERN_ALERT "failed to update recv state");
			// XXX how do we fail here?
		}

		// 6) If this was a nonblocking call and we still don't have any
		//    additional bytes to forward, break out early
		if (nonblock && conn_state->ops->num_recv_bytes_to_forward(conn_state->recv_state) == 0) {
			printk(KERN_ALERT "returning at nonb with %d", bytes_sent);
			return bytes_sent > 0 ? bytes_sent : -EAGAIN;
		}

		// 7) Otherwise if this was a blocking call keep trying until we have
		//    at least something to send back
	}

	// 8) copy to user what we received. return total number bytes sent
	b_to_forward = conn_state->ops->num_recv_bytes_to_forward(conn_state->recv_state);
	bytes_to_copy = b_to_forward > len - bytes_sent ? len - bytes_sent : b_to_forward;
	if (conn_state->ops->copy_to_user(conn_state->recv_state, (void __user *)msg->msg_iov->iov_base + bytes_sent, bytes_to_copy) != 0) {
		printk(KERN_ALERT "failed to copy what we wanted to");
		// XXX how do we fail here?
	}
	conn_state->ops->inc_recv_bytes_forwarded(conn_state->recv_state, bytes_to_copy);
	bytes_sent += bytes_to_copy;
	printk(KERN_ALERT "returning at end with %d", bytes_sent);
	return bytes_sent;

/*
	// Early breakout if we aren't monitoring this connection
	if ((conn_state = th_conn_state_get(current->pid, sock)) == NULL) {
		ret = ref_tcp_recvmsg(iocb, sk, msg, len, nonblock, flags, addr_len);
		return ret;
	}

	buffer = kmalloc(len, GFP_KERNEL);
	oldfs = get_fs();
	set_fs(KERNEL_DS);
	
	//kmsg.msg_control = NULL;
	//kmsg.msg_controllen = 0;
	//kmsg.msg_iovlen = 1;
	//kmsg.msg_iov = &iov;
	//iov.iov_len = len;
	//iov.iov_base = buffer;
	//kmsg.msg_name = 0;
	//kmsg.msg_namelen = 0;
	kmsg = *msg;
	kmsg.msg_iov = &iov;
	iov.iov_len = len;
	iov.iov_base = buffer;

	ret = ref_tcp_recvmsg(iocb, sk, &kmsg, len, nonblock, flags, addr_len);
	if (ret == -EIOCBQUEUED) {
		ret = wait_on_sync_kiocb(iocb);
	}

	set_fs(oldfs);
	if (ret <= 0) {
		kfree(buffer);
		return ret;
	}

	// F UP ANYTHING YOU WANT.  IT'S YOURS.
	if (th_parse_comm(current->pid, sock, (char*)buffer, ret, TH_RECV) < 0) {
		th_conn_state_delete(current->pid, sock);
		return 0;
	}
	if (conn_state->recv_state.state == IRRELEVANT) {
		print_call_info(sock, "No longer interested in socket, ceasing monitoring");
		th_conn_state_delete(current->pid, sock); 
	}

	if (copy_to_user((void __user *)msg->msg_iov->iov_base, buffer, len) != 0) {
		printk(KERN_ALERT "yikes! couldn't copy all the data!");
	}
	//printk(KERN_INFO "After:");
	//printk(KERN_INFO "recv returned: %s", (char*)(buffer));
	kfree(buffer);

	return ret;
*/
}

